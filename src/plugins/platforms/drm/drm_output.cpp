/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_output.h"
#include "drm_backend.h"
#include "drm_object_crtc.h"
#include "drm_object_connector.h"
#include "edid.h"

#include "composite.h"
#include "cursor.h"
#include "logging.h"
#include "main.h"
#include "renderloop.h"
#include "screens.h"
#include "session.h"
#include "wayland_server.h"
// KWayland
#include <KWaylandServer/output_interface.h>
// Qt
#include <QMatrix4x4>
#include <QCryptographicHash>
#include <QPainter>
// c++
#include <cerrno>
// drm
#include <xf86drm.h>
#include <libdrm/drm_mode.h>

#include "drm_gpu.h"

namespace KWin
{

DrmOutput::DrmOutput(DrmBackend *backend, DrmGpu *gpu)
    : AbstractWaylandOutput(backend)
    , m_backend(backend)
    , m_gpu(gpu)
    , m_renderLoop(new RenderLoop(this))
{
}

DrmOutput::~DrmOutput()
{
    Q_ASSERT(!m_pageFlipPending);
    teardown();
}

RenderLoop *DrmOutput::renderLoop() const
{
    return m_renderLoop;
}

void DrmOutput::teardown()
{
    if (m_deleted) {
        return;
    }
    m_deleted = true;
    hideCursor();
    m_crtc->blank(this);

    if (m_primaryPlane) {
        // TODO: when having multiple planes, also clean up these
        m_primaryPlane->setCurrent(nullptr);
    }

    m_cursor[0].reset(nullptr);
    m_cursor[1].reset(nullptr);
    if (!m_pageFlipPending) {
        deleteLater();
    } //else will be deleted in the page flip handler
    //this is needed so that the pageflipcallback handle isn't deleted
}

void DrmOutput::releaseGbm()
{
    if (const auto &b = m_crtc->current()) {
        b->releaseGbm();
    }
    if (m_primaryPlane && m_primaryPlane->current()) {
        m_primaryPlane->current()->releaseGbm();
    }
}

bool DrmOutput::hideCursor()
{
    return drmModeSetCursor(m_gpu->fd(), m_crtc->id(), 0, 0, 0) == 0;
}

bool DrmOutput::showCursor(DrmDumbBuffer *c)
{
    const QSize &s = c->size();
    return drmModeSetCursor(m_gpu->fd(), m_crtc->id(), c->handle(), s.width(), s.height()) == 0;
}

bool DrmOutput::showCursor()
{
    if (m_deleted) {
        return false;
    }

    const bool ret = showCursor(m_cursor[m_cursorIndex].data());
    if (!ret) {
        qCDebug(KWIN_DRM) << "DrmOutput::showCursor(DrmDumbBuffer) failed";
        return ret;
    }

    if (m_hasNewCursor) {
        m_cursorIndex = (m_cursorIndex + 1) % 2;
        m_hasNewCursor = false;
    }

    return ret;
}

static bool isCursorSpriteCompatible(const QImage *buffer, const QImage *sprite)
{
    // Note that we need compare the rects in the device independent pixels because the
    // buffer and the cursor sprite image may have different scale factors.
    const QRect bufferRect(QPoint(0, 0), buffer->size() / buffer->devicePixelRatio());
    const QRect spriteRect(QPoint(0, 0), sprite->size() / sprite->devicePixelRatio());

    return bufferRect.contains(spriteRect);
}

bool DrmOutput::updateCursor()
{
    if (m_deleted) {
        return false;
    }
    const Cursor *cursor = Cursors::self()->currentCursor();
    const QImage cursorImage = cursor->image();
    if (cursorImage.isNull()) {
        return false;
    }

    QImage *c = m_cursor[m_cursorIndex]->image();
    c->setDevicePixelRatio(scale());

    if (!isCursorSpriteCompatible(c, &cursorImage)) {
        // If the cursor image is too big, fall back to rendering the software cursor.
        return false;
    }

    m_hasNewCursor = true;
    c->fill(Qt::transparent);

    QPainter p;
    p.begin(c);
    p.setWorldTransform(logicalToNativeMatrix(cursor->rect(), 1, transform()).toTransform());
    p.drawImage(QPoint(0, 0), cursorImage);
    p.end();

    return true;
}

void DrmOutput::moveCursor()
{
    Cursor *cursor = Cursors::self()->currentCursor();
    const QMatrix4x4 hotspotMatrix = logicalToNativeMatrix(cursor->rect(), scale(), transform());
    const QMatrix4x4 monitorMatrix = logicalToNativeMatrix(geometry(), scale(), transform());

    QPoint pos = monitorMatrix.map(cursor->pos());
    pos -= hotspotMatrix.map(cursor->hotspot());

    drmModeMoveCursor(m_gpu->fd(), m_crtc->id(), pos.x(), pos.y());
}

namespace {
quint64 refreshRateForMode(_drmModeModeInfo *m)
{
    // Calculate higher precision (mHz) refresh rate
    // logic based on Weston, see compositor-drm.c
    quint64 refreshRate = (m->clock * 1000000LL / m->htotal + m->vtotal / 2) / m->vtotal;
    if (m->flags & DRM_MODE_FLAG_INTERLACE) {
        refreshRate *= 2;
    }
    if (m->flags & DRM_MODE_FLAG_DBLSCAN) {
        refreshRate /= 2;
    }
    if (m->vscan > 1) {
        refreshRate /= m->vscan;
    }
    return refreshRate;
}
}

bool DrmOutput::init(drmModeConnector *connector)
{
    initUuid();
    if (m_gpu->atomicModeSetting() && !m_primaryPlane) {
        return false;
    }

    setInternal(m_conn->isInternal());
    setDpmsSupported(true);
    initOutputDevice(connector);

    if (!m_gpu->atomicModeSetting() && !m_crtc->blank(this)) {
        // We use legacy mode and the initial output blank failed.
        return false;
    }

    updateDpms(KWaylandServer::OutputInterface::DpmsMode::On);
    return true;
}

void DrmOutput::initUuid()
{
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(QByteArray::number(m_conn->id()));
    hash.addData(m_conn->edid()->eisaId());
    hash.addData(m_conn->edid()->monitorName());
    hash.addData(m_conn->edid()->serialNumber());
    m_uuid = hash.result().toHex().left(10);
}

void DrmOutput::initOutputDevice(drmModeConnector *connector)
{
    // read in mode information
    QVector<KWaylandServer::OutputDeviceInterface::Mode> modes;
    for (int i = 0; i < connector->count_modes; ++i) {
        // TODO: in AMS here we could read and store for later every mode's blob_id
        // would simplify isCurrentMode(..) and presentAtomically(..) in case of mode set
        auto *m = &connector->modes[i];
        KWaylandServer::OutputDeviceInterface::ModeFlags deviceflags;
        if (isCurrentMode(m)) {
            deviceflags |= KWaylandServer::OutputDeviceInterface::ModeFlag::Current;
        }
        if (m->type & DRM_MODE_TYPE_PREFERRED) {
            deviceflags |= KWaylandServer::OutputDeviceInterface::ModeFlag::Preferred;
        }

        KWaylandServer::OutputDeviceInterface::Mode mode;
        mode.id = i;
        mode.size = QSize(m->hdisplay, m->vdisplay);
        mode.flags = deviceflags;
        mode.refreshRate = refreshRateForMode(m);
        modes << mode;
    }

    setName(m_conn->connectorName());
    initInterfaces(m_conn->modelName(), m_conn->edid()->manufacturerString(), m_uuid, m_conn->physicalSize(), modes, m_conn->edid()->raw());
}

bool DrmOutput::isCurrentMode(const drmModeModeInfo *mode) const
{
    return mode->clock       == m_mode.clock
        && mode->hdisplay    == m_mode.hdisplay
        && mode->hsync_start == m_mode.hsync_start
        && mode->hsync_end   == m_mode.hsync_end
        && mode->htotal      == m_mode.htotal
        && mode->hskew       == m_mode.hskew
        && mode->vdisplay    == m_mode.vdisplay
        && mode->vsync_start == m_mode.vsync_start
        && mode->vsync_end   == m_mode.vsync_end
        && mode->vtotal      == m_mode.vtotal
        && mode->vscan       == m_mode.vscan
        && mode->vrefresh    == m_mode.vrefresh
        && mode->flags       == m_mode.flags
        && mode->type        == m_mode.type
        && qstrcmp(mode->name, m_mode.name) == 0;
}

bool DrmOutput::initCursor(const QSize &cursorSize)
{
    auto createCursor = [this, cursorSize] (int index) {
        m_cursor[index].reset(new DrmDumbBuffer(m_gpu, cursorSize));
        if (!m_cursor[index]->map(QImage::Format_ARGB32_Premultiplied)) {
            return false;
        }
        return true;
    };
    if (!createCursor(0) || !createCursor(1)) {
        return false;
    }
    return true;
}

void DrmOutput::updateEnablement(bool enable)
{
    if (enable) {
        m_dpmsModePending = DpmsMode::On;
        if (m_gpu->atomicModeSetting()) {
            atomicEnable();
        } else {
            if (dpmsLegacyApply()) {
                m_backend->enableOutput(this, true);
            }
        }

    } else {
        m_dpmsModePending = DpmsMode::Off;
        if (m_gpu->atomicModeSetting()) {
            atomicDisable();
        } else {
            if (dpmsLegacyApply()) {
                m_backend->enableOutput(this, false);
            }
        }
    }
}

void DrmOutput::atomicEnable()
{
    m_modesetRequested = true;

    if (m_atomicOffPending) {
        Q_ASSERT(m_pageFlipPending);
        m_atomicOffPending = false;
    }
    m_backend->enableOutput(this, true);

    if (Compositor *compositor = Compositor::self()) {
        compositor->addRepaintFull();
    }
}

void DrmOutput::atomicDisable()
{
    m_modesetRequested = true;

    m_backend->enableOutput(this, false);
    m_atomicOffPending = true;
    if (!m_pageFlipPending) {
        dpmsAtomicOff();
    }
}

static DrmOutput::DpmsMode fromWaylandDpmsMode(KWaylandServer::OutputInterface::DpmsMode wlMode)
{
    using namespace KWaylandServer;
    switch (wlMode) {
    case OutputInterface::DpmsMode::On:
        return DrmOutput::DpmsMode::On;
    case OutputInterface::DpmsMode::Standby:
        return DrmOutput::DpmsMode::Standby;
    case OutputInterface::DpmsMode::Suspend:
        return DrmOutput::DpmsMode::Suspend;
    case OutputInterface::DpmsMode::Off:
        return DrmOutput::DpmsMode::Off;
    default:
        Q_UNREACHABLE();
    }
}

static KWaylandServer::OutputInterface::DpmsMode toWaylandDpmsMode(DrmOutput::DpmsMode mode)
{
    using namespace KWaylandServer;
    switch (mode) {
    case DrmOutput::DpmsMode::On:
        return OutputInterface::DpmsMode::On;
    case DrmOutput::DpmsMode::Standby:
        return OutputInterface::DpmsMode::Standby;
    case DrmOutput::DpmsMode::Suspend:
        return OutputInterface::DpmsMode::Suspend;
    case DrmOutput::DpmsMode::Off:
        return OutputInterface::DpmsMode::Off;
    default:
        Q_UNREACHABLE();
    }
}

void DrmOutput::updateDpms(KWaylandServer::OutputInterface::DpmsMode mode)
{
    if (!m_conn->dpms() || !isEnabled()) {
        return;
    }

    const auto drmMode = fromWaylandDpmsMode(mode);

    if (drmMode == m_dpmsModePending) {
        qCDebug(KWIN_DRM) << "New DPMS mode equals old mode. DPMS unchanged.";
        waylandOutput()->setDpmsMode(mode);
        return;
    }

    m_dpmsModePending = drmMode;

    if (m_gpu->atomicModeSetting()) {
        m_modesetRequested = true;
        if (drmMode == DpmsMode::On) {
            if (m_atomicOffPending) {
                Q_ASSERT(m_pageFlipPending);
                m_atomicOffPending = false;
            }
            dpmsFinishOn();
        } else {
            m_atomicOffPending = true;
            if (!m_pageFlipPending) {
                dpmsAtomicOff();
            }
        }
    } else {
       dpmsLegacyApply();
    }
}

void DrmOutput::dpmsFinishOn()
{
    qCDebug(KWIN_DRM) << "DPMS mode set for output" << m_crtc->id() << "to On.";

    waylandOutput()->setDpmsMode(toWaylandDpmsMode(DpmsMode::On));

    m_backend->checkOutputsAreOn();
    m_crtc->blank(this);
    m_renderLoop->uninhibit();
    if (Compositor *compositor = Compositor::self()) {
        compositor->addRepaintFull();
    }
}

void DrmOutput::dpmsFinishOff()
{
    qCDebug(KWIN_DRM) << "DPMS mode set for output" << m_crtc->id() << "to Off.";

    if (isEnabled()) {
        waylandOutput()->setDpmsMode(toWaylandDpmsMode(m_dpmsModePending));
        m_backend->createDpmsFilter();
    } else {
        waylandOutput()->setDpmsMode(toWaylandDpmsMode(DpmsMode::Off));
    }
    m_renderLoop->inhibit();
}

bool DrmOutput::dpmsLegacyApply()
{
    if (drmModeConnectorSetProperty(m_gpu->fd(), m_conn->id(),
                                    m_conn->dpms()->propId(), uint64_t(m_dpmsModePending)) < 0) {
        m_dpmsModePending = m_dpmsMode;
        qCWarning(KWIN_DRM) << "Setting DPMS failed";
        return false;
    }
    if (m_dpmsModePending == DpmsMode::On) {
        dpmsFinishOn();
    } else {
        dpmsFinishOff();
    }
    m_dpmsMode = m_dpmsModePending;
    return true;
}

DrmPlane::Transformations outputToPlaneTransform(DrmOutput::Transform transform)
 {
    using OutTrans = DrmOutput::Transform;
    using PlaneTrans = DrmPlane::Transformation;

     // TODO: Do we want to support reflections (flips)?

     switch (transform) {
    case OutTrans::Normal:
    case OutTrans::Flipped:
        return PlaneTrans::Rotate0;
    case OutTrans::Rotated90:
    case OutTrans::Flipped90:
        return PlaneTrans::Rotate90;
    case OutTrans::Rotated180:
    case OutTrans::Flipped180:
        return PlaneTrans::Rotate180;
    case OutTrans::Rotated270:
    case OutTrans::Flipped270:
        return PlaneTrans::Rotate270;
     default:
         Q_UNREACHABLE();
     }
}

bool DrmOutput::hardwareTransforms() const
{
    if (!m_primaryPlane) {
        return false;
    }
    return m_primaryPlane->transformation() == outputToPlaneTransform(transform());
}

void DrmOutput::updateTransform(Transform transform)
{
    const auto planeTransform = outputToPlaneTransform(transform);

     if (m_primaryPlane) {
        // At the moment we have to exclude hardware transforms for vertical buffers.
        // For that we need to support other buffers and graceful fallback from atomic tests.
        // Reason is that standard linear buffers are not suitable.
        const bool isPortrait = transform == Transform::Rotated90
                                || transform == Transform::Flipped90
                                || transform == Transform::Rotated270
                                || transform == Transform::Flipped270;

        if (!qEnvironmentVariableIsSet("KWIN_DRM_SW_ROTATIONS_ONLY") &&
                (m_primaryPlane->supportedTransformations() & planeTransform) &&
                !isPortrait) {
            m_primaryPlane->setTransformation(planeTransform);
        } else {
            m_primaryPlane->setTransformation(DrmPlane::Transformation::Rotate0);
        }
    }
    m_modesetRequested = true;

    // show cursor only if is enabled, i.e if pointer device is presentP
    if (!m_backend->isCursorHidden() && !m_backend->usesSoftwareCursor()) {
        // the cursor might need to get rotated
        updateCursor();
        showCursor();
    }
}

void DrmOutput::updateMode(uint32_t width, uint32_t height, uint32_t refreshRate)
{
    if (m_mode.hdisplay == width && m_mode.vdisplay == height && m_mode.vrefresh == refreshRate) {
        return;
    }
    // try to find a fitting mode
    DrmScopedPointer<drmModeConnector> connector(drmModeGetConnectorCurrent(m_gpu->fd(), m_conn->id()));
    for (int i = 0; i < connector->count_modes; i++) {
        auto mode = connector->modes[i];
        if (mode.hdisplay == width && mode.vdisplay == height && mode.vrefresh == refreshRate) {
            updateMode(i);
            return;
        }
    }
    qCWarning(KWIN_DRM, "Could not find a fitting mode with size=%dx%d and refresh rate %d for output %s",
              width, height, refreshRate, uuid().constData());
}

void DrmOutput::updateMode(int modeIndex)
{
    // get all modes on the connector
    DrmScopedPointer<drmModeConnector> connector(drmModeGetConnector(m_gpu->fd(), m_conn->id()));
    if (connector->count_modes <= modeIndex) {
        // TODO: error?
        return;
    }
    if (isCurrentMode(&connector->modes[modeIndex])) {
        // nothing to do
        return;
    }
    m_mode = connector->modes[modeIndex];
    m_modesetRequested = true;
    setWaylandMode();
}

void DrmOutput::setWaylandMode()
{
    AbstractWaylandOutput::setWaylandMode(QSize(m_mode.hdisplay, m_mode.vdisplay),
                                          refreshRateForMode(&m_mode));
}

void DrmOutput::pageFlipped()
{
    // In legacy mode we might get a page flip through a blank.
    Q_ASSERT(m_pageFlipPending || !m_gpu->atomicModeSetting());
    m_pageFlipPending = false;

    if (m_deleted) {
        deleteLater();
        return;
    }

    if (!m_crtc) {
        return;
    }
    if (m_gpu->atomicModeSetting()) {
        if (!m_primaryPlane->next()) {
            if (m_primaryPlane->current()) {
                m_primaryPlane->current()->releaseGbm();
            }
            return;
        }
        for (DrmPlane *p : m_nextPlanesFlipList) {
            p->flipBuffer();
        }
        m_nextPlanesFlipList.clear();
    } else {
        if (!m_crtc->next()) {
            // on manual vt switch
            if (const auto &b = m_crtc->current()) {
                b->releaseGbm();
            }
        }
        m_crtc->flipBuffer();
    }

    if (m_atomicOffPending) {
        dpmsAtomicOff();
    }
}

bool DrmOutput::present(const QSharedPointer<DrmBuffer> &buffer)
{
    if (!buffer || buffer->bufferId() == 0) {
        return false;
    }
    if (m_dpmsModePending != DpmsMode::On) {
        return false;
    }
    return m_gpu->atomicModeSetting() ? presentAtomically(buffer) : presentLegacy(buffer);
}

bool DrmOutput::dpmsAtomicOff()
{
    m_atomicOffPending = false;

    // TODO: With multiple planes: deactivate all of them here
    m_primaryPlane->setNext(nullptr);
    m_nextPlanesFlipList << m_primaryPlane;

    if (!doAtomicCommit(AtomicCommitMode::Test)) {
        qCDebug(KWIN_DRM) << "Atomic test commit to Dpms Off failed. Aborting.";
        return false;
    }
    if (!doAtomicCommit(AtomicCommitMode::Real)) {
        qCDebug(KWIN_DRM) << "Atomic commit to Dpms Off failed. This should have never happened! Aborting.";
        return false;
    }
    m_nextPlanesFlipList.clear();
    dpmsFinishOff();

    return true;
}

bool DrmOutput::presentAtomically(const QSharedPointer<DrmBuffer> &buffer)
{
    if (!m_backend->session()->isActive()) {
        qCWarning(KWIN_DRM) << "Refusing to present output because session is inactive";
        return false;
    }

    if (m_pageFlipPending) {
        qCWarning(KWIN_DRM) << "Page not yet flipped.";
        return false;
    }

#if HAVE_EGL_STREAMS
    if (m_gpu->useEglStreams() && !m_modesetRequested) {
        // EglStreamBackend queues normal page flips through EGL,
        // modesets are still performed through DRM-KMS
        m_pageFlipPending = true;
        return true;
    }
#endif

    m_primaryPlane->setNext(buffer);
    m_nextPlanesFlipList << m_primaryPlane;

    if (!doAtomicCommit(AtomicCommitMode::Test)) {
        //TODO: When we use planes for layered rendering, fallback to renderer instead. Also for direct scanout?
        //TODO: Probably should undo setNext and reset the flip list
        qCDebug(KWIN_DRM) << "Atomic test commit failed. Aborting present.";
        // go back to previous state
        if (m_lastWorkingState.valid) {
            m_mode = m_lastWorkingState.mode;
            setTransform(m_lastWorkingState.transform);
            setGlobalPos(m_lastWorkingState.globalPos);
            if (m_primaryPlane) {
                m_primaryPlane->setTransformation(m_lastWorkingState.planeTransformations);
            }
            m_modesetRequested = true;
            if (!m_backend->isCursorHidden()) {
                // the cursor might need to get rotated
                updateCursor();
                showCursor();
            }
            setWaylandMode();
            emit screens()->changed();
        }
        return false;
    }
    const bool wasModeset = m_modesetRequested;
    if (!doAtomicCommit(AtomicCommitMode::Real)) {
        qCDebug(KWIN_DRM) << "Atomic commit failed. This should have never happened! Aborting present.";
        //TODO: Probably should undo setNext and reset the flip list
        return false;
    }
    if (wasModeset) {
        // store current mode set as new good state
        m_lastWorkingState.mode = m_mode;
        m_lastWorkingState.transform = transform();
        m_lastWorkingState.globalPos = globalPos();
        if (m_primaryPlane) {
            m_lastWorkingState.planeTransformations = m_primaryPlane->transformation();
        }
        m_lastWorkingState.valid = true;
        m_renderLoop->setRefreshRate(refreshRateForMode(&m_mode));
    }
    m_pageFlipPending = true;
    return true;
}

bool DrmOutput::presentLegacy(const QSharedPointer<DrmBuffer> &buffer)
{
    if (m_crtc->next()) {
        return false;
    }
    if (!m_backend->session()->isActive()) {
        m_crtc->setNext(buffer);
        return false;
    }

    // Do we need to set a new mode first?
    if (!m_crtc->current() || m_crtc->current()->needsModeChange(buffer.get())) {
        if (!setModeLegacy(buffer.get())) {
            return false;
        }
    }
    const bool ok = drmModePageFlip(m_gpu->fd(), m_crtc->id(), buffer->bufferId(), DRM_MODE_PAGE_FLIP_EVENT, this) == 0;
    if (ok) {
        m_crtc->setNext(buffer);
        m_pageFlipPending = true;
    } else {
        qCWarning(KWIN_DRM) << "Page flip failed:" << strerror(errno);
    }
    return ok;
}

bool DrmOutput::setModeLegacy(DrmBuffer *buffer)
{
    uint32_t connId = m_conn->id();
    if (drmModeSetCrtc(m_gpu->fd(), m_crtc->id(), buffer->bufferId(), 0, 0, &connId, 1, &m_mode) == 0) {
        return true;
    } else {
        qCWarning(KWIN_DRM) << "Mode setting failed";
        return false;
    }
}

bool DrmOutput::doAtomicCommit(AtomicCommitMode mode)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();

    auto errorHandler = [this, mode, req] () {
        if (mode == AtomicCommitMode::Test) {
            // TODO: when we later test overlay planes, make sure we change only the right stuff back
        }
        if (req) {
            drmModeAtomicFree(req);
        }

        if (m_dpmsMode != m_dpmsModePending) {
            qCWarning(KWIN_DRM) << "Setting DPMS failed";
            m_dpmsModePending = m_dpmsMode;
            if (m_dpmsMode != DpmsMode::On) {
                dpmsFinishOff();
            }
        }

        // TODO: see above, rework later for overlay planes!
        for (DrmPlane *p : m_nextPlanesFlipList) {
            p->setNext(nullptr);
        }
        m_nextPlanesFlipList.clear();

    };

    if (!req) {
        qCWarning(KWIN_DRM) << "DRM: couldn't allocate atomic request";
        errorHandler();
        return false;
    }

    uint32_t flags = 0;
    // Do we need to set a new mode?
    if (m_modesetRequested) {
        if (m_dpmsModePending == DpmsMode::On) {
            if (drmModeCreatePropertyBlob(m_gpu->fd(), &m_mode, sizeof(m_mode), &m_blobId) != 0) {
                qCWarning(KWIN_DRM) << "Failed to create property blob";
                errorHandler();
                return false;
            }
        }
        if (!atomicReqModesetPopulate(req, m_dpmsModePending == DpmsMode::On)){
            qCWarning(KWIN_DRM) << "Failed to populate Atomic Modeset";
            errorHandler();
            return false;
        }
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    }

    if (mode == AtomicCommitMode::Real) {
        if (m_dpmsModePending == DpmsMode::On) {
            if (!(flags & DRM_MODE_ATOMIC_ALLOW_MODESET)) {
                // TODO: Evaluating this condition should only be necessary, as long as we expect older kernels than 4.10.
                flags |= DRM_MODE_ATOMIC_NONBLOCK;
            }

#if HAVE_EGL_STREAMS
            if (!m_gpu->useEglStreams())
                // EglStreamBackend uses the NV_output_drm_flip_event EGL extension
                // to register the flip event through eglStreamConsumerAcquireAttribNV
#endif
                flags |= DRM_MODE_PAGE_FLIP_EVENT;
        }
    } else {
        flags |= DRM_MODE_ATOMIC_TEST_ONLY;
    }

    bool ret = true;
    // TODO: Make sure when we use more than one plane at a time, that we go through this list in the right order.
    for (int i = m_nextPlanesFlipList.size() - 1; 0 <= i; i-- ) {
        DrmPlane *p = m_nextPlanesFlipList[i];
        ret &= p->atomicPopulate(req);
    }

    if (!ret) {
        qCWarning(KWIN_DRM) << "Failed to populate atomic planes. Abort atomic commit!";
        errorHandler();
        return false;
    }

    if (drmModeAtomicCommit(m_gpu->fd(), req, flags, this)) {
        qCDebug(KWIN_DRM) << "Atomic request failed to commit: " << strerror(errno);
        errorHandler();
        return false;
    }

    if (mode == AtomicCommitMode::Real && (flags & DRM_MODE_ATOMIC_ALLOW_MODESET)) {
        qCDebug(KWIN_DRM) << "Atomic Modeset successful.";
        m_modesetRequested = false;
        m_dpmsMode = m_dpmsModePending;
    }

    drmModeAtomicFree(req);
    return true;
}

bool DrmOutput::atomicReqModesetPopulate(drmModeAtomicReq *req, bool enable)
{
    if (enable) {
        const QSize mSize = modeSize();
        const QSize bufferSize = m_primaryPlane->next() ? m_primaryPlane->next()->size() : pixelSize();
        const QSize sourceSize = hardwareTransforms() ? bufferSize : mSize;
        QRect targetRect = QRect(QPoint(0, 0), mSize);
        if (mSize != sourceSize) {
            targetRect.setSize(sourceSize.scaled(mSize, Qt::AspectRatioMode::KeepAspectRatio));
            targetRect.setX((mSize.width() - targetRect.width()) / 2);
            targetRect.setY((mSize.height() - targetRect.height()) / 2);
        }

        m_primaryPlane->setValue(DrmPlane::PropertyIndex::SrcX, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::SrcY, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::SrcW, sourceSize.width() << 16);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::SrcH, sourceSize.height() << 16);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcX, targetRect.x());
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcY, targetRect.y());
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcW, targetRect.width());
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcH, targetRect.height());
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcId, m_crtc->id());
    } else {
        m_primaryPlane->setCurrent(nullptr);
        m_primaryPlane->setNext(nullptr);

        m_primaryPlane->setValue(DrmPlane::PropertyIndex::SrcX, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::SrcY, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::SrcW, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::SrcH, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcX, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcY, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcW, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcH, 0);
        m_primaryPlane->setValue(DrmPlane::PropertyIndex::CrtcId, 0);
    }
    m_conn->setValue(DrmConnector::PropertyIndex::CrtcId, enable ? m_crtc->id() : 0);
    m_crtc->setValue(DrmCrtc::PropertyIndex::ModeId, enable ? m_blobId : 0);
    m_crtc->setValue(DrmCrtc::PropertyIndex::Active, enable);

    bool ret = true;
    ret &= m_conn->atomicPopulate(req);
    ret &= m_crtc->atomicPopulate(req);

    return ret;
}

int DrmOutput::gammaRampSize() const
{
    return m_crtc->gammaRampSize();
}

bool DrmOutput::setGammaRamp(const GammaRamp &gamma)
{
    return m_crtc->setGammaRamp(gamma);
}

}

QDebug& operator<<(QDebug& s, const KWin::DrmOutput *output)
{
    if (!output)
        return s.nospace() << "DrmOutput()";
    return s.nospace() << "DrmOutput(" << output->name() << ", crtc:" << output->crtc() << ", connector:" << output->connector() << ", geometry:" << output->geometry() << ')';
}