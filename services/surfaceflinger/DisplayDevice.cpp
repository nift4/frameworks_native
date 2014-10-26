/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "DisplayDevice"

#include <android-base/stringprintf.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/Display.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/DisplayColorProfileCreationArgs.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/DisplaySurface.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/RenderSurfaceCreationArgs.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <configstore/Utils.h>
#include <log/log.h>
#include <system/window.h>
#include <ui/GraphicTypes.h>

#include <fcntl.h>
#include <termios.h>
#include <linux/kd.h>
#include <linux/vt.h>

#include "DisplayDevice.h"
#include "Layer.h"
#include "SurfaceFlinger.h"
#include "gralloc_drm.h"

namespace android {

using android::base::StringAppendF;

#ifdef CONSOLE_MANAGER
class ConsoleManagerThread : public Thread {
public:
            ConsoleManagerThread(const sp<SurfaceFlinger>&, const wp<IBinder>&);
    virtual ~ConsoleManagerThread();

    status_t releaseScreen() const;

private:
    sp<SurfaceFlinger> mFlinger;
    wp<IBinder> mDisplayToken;
    int consoleFd;
    long prev_vt_num;
    vt_mode vm;
    virtual void onFirstRef();
    virtual status_t readyToRun();
    virtual void requestExit();
    virtual bool threadLoop();
    static void sigHandler(int sig);
    static pid_t sSignalCatcherPid;
};

ConsoleManagerThread::ConsoleManagerThread(const sp<SurfaceFlinger>& flinger, const wp<IBinder>& token)
    : Thread(false), mFlinger(flinger), mDisplayToken(token), consoleFd(-1)
{
    sSignalCatcherPid = 0;

    // create a new console
    char const * const ttydev = "/dev/tty0";
    int fd = open(ttydev, O_RDWR | O_SYNC);
    if (fd < 0) {
        ALOGE("Can't open %s, errno=%d (%s)", ttydev, errno, strerror(errno));
        consoleFd = -errno;
        return;
    }
    ALOGD("Open /dev/tty0 OK");

    // to make sure that we are in text mode
    int res = ioctl(fd, KDSETMODE, (void*) KD_TEXT);
    if (res < 0) {
        ALOGE("ioctl(%d, KDSETMODE, ...) failed, res %d (%s)",
                fd, res, strerror(errno));
    }

    // get the current console
    struct vt_stat vs;
    res = ioctl(fd, VT_GETSTATE, &vs);
    if (res < 0) {
        ALOGE("ioctl(%d, VT_GETSTATE, ...) failed, res %d (%s)",
                fd, res, strerror(errno));
        consoleFd = -errno;
        return;
    }

    // switch to console 7 (which is what X normaly uses)
    do {
        res = ioctl(fd, VT_ACTIVATE, ANDROID_VT);
    } while(res < 0 && errno == EINTR);
    if (res < 0) {
        ALOGE("ioctl(%d, VT_ACTIVATE, ...) failed, %d (%s) for vt %d",
                fd, errno, strerror(errno), ANDROID_VT);
        consoleFd = -errno;
        return;
    }

    do {
        res = ioctl(fd, VT_WAITACTIVE, ANDROID_VT);
    } while (res < 0 && errno == EINTR);
    if (res < 0) {
        ALOGE("ioctl(%d, VT_WAITACTIVE, ...) failed, %d %d %s for vt %d",
                fd, res, errno, strerror(errno), ANDROID_VT);
        consoleFd = -errno;
        return;
    }

    // open the new console
    close(fd);
    fd = open(ttydev, O_RDWR | O_SYNC);
    if (fd < 0) {
        ALOGE("Can't open new console %s", ttydev);
        consoleFd = -errno;
        return;
    }

    /* disable console line buffer, echo, ... */
    struct termios ttyarg;
    ioctl(fd, TCGETS , &ttyarg);
    ttyarg.c_iflag = 0;
    ttyarg.c_lflag = 0;
    ioctl(fd, TCSETS , &ttyarg);

    // set up signals so we're notified when the console changes
    // we can't use SIGUSR1 because it's used by the java-vm
    vm.mode = VT_PROCESS;
    vm.waitv = 0;
    vm.relsig = SIGUSR2;
    vm.acqsig = SIGUNUSED;
    vm.frsig = 0;

    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_handler = sigHandler;
    act.sa_flags = 0;
    sigaction(vm.relsig, &act, NULL);

    sigemptyset(&act.sa_mask);
    act.sa_handler = sigHandler;
    act.sa_flags = 0;
    sigaction(vm.acqsig, &act, NULL);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, vm.relsig);
    sigaddset(&mask, vm.acqsig);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    // switch to graphic mode
    res = ioctl(fd, KDSETMODE, (void*)KD_GRAPHICS);
    ALOGW_IF(res < 0,
            "ioctl(%d, KDSETMODE, KD_GRAPHICS) failed, res %d", fd, res);

    prev_vt_num = vs.v_active;
    consoleFd = fd;
}

ConsoleManagerThread::~ConsoleManagerThread()
{
    if (consoleFd >= 0) {
        int fd = consoleFd;
        int res;
        ioctl(fd, KDSETMODE, (void*)KD_TEXT);
        do {
            res = ioctl(fd, VT_ACTIVATE, prev_vt_num);
        } while(res < 0 && errno == EINTR);
        do {
            res = ioctl(fd, VT_WAITACTIVE, prev_vt_num);
        } while(res < 0 && errno == EINTR);
        close(fd);
        char const * const ttydev = "/dev/tty0";
        fd = open(ttydev, O_RDWR | O_SYNC);
        ioctl(fd, VT_DISALLOCATE, 0);
        close(fd);
    }
}

status_t ConsoleManagerThread::releaseScreen() const
{
    int err = ioctl(consoleFd, VT_RELDISP, (void*)1);
    ALOGE_IF(err < 0, "ioctl(%d, VT_RELDISP, 1) failed %d (%s)",
        consoleFd, errno, strerror(errno));
    return (err < 0) ? (-errno) : status_t(NO_ERROR);
}

void ConsoleManagerThread::onFirstRef()
{
    run("ConsoleManagerThread", PRIORITY_URGENT_DISPLAY);
}

status_t ConsoleManagerThread::readyToRun()
{
    if (consoleFd >= 0) {
        sSignalCatcherPid = gettid();

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, vm.relsig);
        sigaddset(&mask, vm.acqsig);
        sigprocmask(SIG_BLOCK, &mask, NULL);

        int res = ioctl(consoleFd, VT_SETMODE, &vm);
        if (res < 0) {
            ALOGE("ioctl(%d, VT_SETMODE, ...) failed, %d (%s)",
                    consoleFd, errno, strerror(errno));
        }
        return NO_ERROR;
    }
    return consoleFd;
}

void ConsoleManagerThread::requestExit()
{
    Thread::requestExit();
    if (sSignalCatcherPid != 0) {
        // wake the thread up
        kill(sSignalCatcherPid, SIGINT);
        // wait for it...
    }
}

bool ConsoleManagerThread::threadLoop()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, vm.relsig);
    sigaddset(&mask, vm.acqsig);

    int sig = 0;
    sigwait(&mask, &sig);

    hw_module_t const* mod;
    gralloc_module_t const* gr = NULL;
    status_t err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &mod);
    if (!err) {
        gr = reinterpret_cast<gralloc_module_t const*>(mod);
        if (!gr->perform)
            gr = NULL;
    }

    if (sig == vm.relsig) {
        if (gr)
            gr->perform(gr, GRALLOC_MODULE_PERFORM_LEAVE_VT);
        mFlinger->screenReleased(mDisplayToken.promote());
    } else if (sig == vm.acqsig) {
        mFlinger->screenAcquired(mDisplayToken.promote());
        if (gr)
            gr->perform(gr, GRALLOC_MODULE_PERFORM_ENTER_VT);
    }

    return true;
}

void ConsoleManagerThread::sigHandler(int sig)
{
    // resend the signal to our signal catcher thread
    ALOGW("received signal %d in thread %d, resending to %d",
            sig, gettid(), sSignalCatcherPid);

    // we absolutely need the delays below because without them
    // our main thread never gets a chance to handle the signal.
    usleep(10000);
    kill(sSignalCatcherPid, sig);
    usleep(10000);
}

pid_t ConsoleManagerThread::sSignalCatcherPid;
#endif

/*
 * Initialize the display to the specified values.
 *
 */

uint32_t DisplayDevice::sPrimaryDisplayOrientation = 0;

DisplayDeviceCreationArgs::DisplayDeviceCreationArgs(const sp<SurfaceFlinger>& flinger,
                                                     const wp<IBinder>& displayToken,
                                                     const std::optional<DisplayId>& displayId)
      : flinger(flinger), displayToken(displayToken), displayId(displayId) {}

DisplayDevice::DisplayDevice(DisplayDeviceCreationArgs&& args)
      : translateX(0), translateY(0),
        mFlinger(args.flinger),
        mDisplayToken(args.displayToken),
        mSequenceId(args.sequenceId),
        mDisplayInstallOrientation(args.displayInstallOrientation),
        mCompositionDisplay{mFlinger->getCompositionEngine().createDisplay(
                compositionengine::DisplayCreationArgs{args.isSecure, args.isVirtual,
                                                       args.displayId})},
        mIsVirtual(args.isVirtual),
#ifdef CONSOLE_MANAGER
        mConsoleManagerThread(0),
#endif        
        mOrientation(),
        mActiveConfig(0),
        mIsPrimary(args.isPrimary),
        mIsPowerModeOverride(false),
        mIsAnimating(false),
        mIsDisplayBuiltInType(false) {
    mCompositionDisplay->createRenderSurface(
            compositionengine::RenderSurfaceCreationArgs{ANativeWindow_getWidth(
                                                                 args.nativeWindow.get()),
                                                         ANativeWindow_getHeight(
                                                                 args.nativeWindow.get()),
                                                         args.nativeWindow, args.displaySurface});

    mCompositionDisplay->createDisplayColorProfile(
            compositionengine::DisplayColorProfileCreationArgs{args.hasWideColorGamut,
                                                               std::move(args.hdrCapabilities),
                                                               args.supportedPerFrameMetadata,
                                                               args.hwcColorModes});

    if (!mCompositionDisplay->isValid()) {
        ALOGE("Composition Display did not validate!");
    }

    mCompositionDisplay->getRenderSurface()->initialize();

    setPowerMode(args.initialPowerMode);

    // initialize the display orientation transform.
    setProjection(DisplayState::eOrientationDefault, Rect::INVALID_RECT, Rect::INVALID_RECT);
}

#ifdef CONSOLE_MANAGER
DisplayDevice::~DisplayDevice() {
    if (mConsoleManagerThread != 0) {
        mConsoleManagerThread->requestExitAndWait();
        ALOGD("ConsoleManagerThread: destroy primary DisplayDevice");
    }
}
#else
DisplayDevice::~DisplayDevice() = default;
#endif

void DisplayDevice::disconnect() {
    mCompositionDisplay->disconnect();
}

int DisplayDevice::getWidth() const {
    return mCompositionDisplay->getState().bounds.getWidth();
}

int DisplayDevice::getHeight() const {
    return mCompositionDisplay->getState().bounds.getHeight();
}

void DisplayDevice::setDisplayName(const std::string& displayName) {
    if (!displayName.empty()) {
        // never override the name with an empty name
        mDisplayName = displayName;
        mCompositionDisplay->setName(displayName);
    }
}

uint32_t DisplayDevice::getPageFlipCount() const {
    return mCompositionDisplay->getRenderSurface()->getPageFlipCount();
}

// ----------------------------------------------------------------------------

void DisplayDevice::setVisibleLayersSortedByZ(const Vector< sp<Layer> >& layers) {
    mVisibleLayersSortedByZ = layers;
}

const Vector< sp<Layer> >& DisplayDevice::getVisibleLayersSortedByZ() const {
    return mVisibleLayersSortedByZ;
}

void DisplayDevice::setLayersNeedingFences(const Vector< sp<Layer> >& layers) {
    mLayersNeedingFences = layers;
}

const Vector< sp<Layer> >& DisplayDevice::getLayersNeedingFences() const {
    return mLayersNeedingFences;
}

// ----------------------------------------------------------------------------
void DisplayDevice::setPowerMode(int mode) {
    mPowerMode = mode;
    getCompositionDisplay()->setCompositionEnabled(mPowerMode != HWC_POWER_MODE_OFF);
#ifdef CONSOLE_MANAGER
    if (mode != HWC_POWER_MODE_NORMAL && mConsoleManagerThread != 0) {
        mConsoleManagerThread->releaseScreen();
    }
#endif    
}

int DisplayDevice::getPowerMode()  const {
    return mPowerMode;
}

bool DisplayDevice::isPoweredOn() const {
    return mPowerMode != HWC_POWER_MODE_OFF;
}

// ----------------------------------------------------------------------------
void DisplayDevice::setActiveConfig(int mode) {
    mActiveConfig = mode;
}

int DisplayDevice::getActiveConfig()  const {
    return mActiveConfig;
}

void DisplayDevice::setPowerModeOverrideConfig(bool supported) {
    mIsPowerModeOverride = supported;
}

bool DisplayDevice::getPowerModeOverrideConfig() const {
    return mIsPowerModeOverride;
}

void DisplayDevice::setAnimating(bool isAnimating) {
    mIsAnimating = isAnimating;
}

bool DisplayDevice::getAnimating() const {
    return mIsAnimating;
}

void DisplayDevice::setIsDisplayBuiltInType(bool isBuiltInType) {
    mIsDisplayBuiltInType = isBuiltInType;
}

bool DisplayDevice::getIsDisplayBuiltInType() const {
    return mIsDisplayBuiltInType;
}

// ----------------------------------------------------------------------------

ui::Dataspace DisplayDevice::getCompositionDataSpace() const {
    return mCompositionDisplay->getState().dataspace;
}

// ----------------------------------------------------------------------------

void DisplayDevice::setLayerStack(uint32_t stack) {
    mCompositionDisplay->setLayerStackFilter(stack, isPrimary());
}

// ----------------------------------------------------------------------------

uint32_t DisplayDevice::displayStateOrientationToTransformOrientation(int orientation) {
    switch (orientation) {
    case DisplayState::eOrientationDefault:
        return ui::Transform::ROT_0;
    case DisplayState::eOrientation90:
        return ui::Transform::ROT_90;
    case DisplayState::eOrientation180:
        return ui::Transform::ROT_180;
    case DisplayState::eOrientation270:
        return ui::Transform::ROT_270;
    default:
        return ui::Transform::ROT_INVALID;
    }
}

status_t DisplayDevice::orientationToTransfrom(int orientation, int w, int h, ui::Transform* tr) {
    uint32_t flags = displayStateOrientationToTransformOrientation(orientation);
    if (flags == ui::Transform::ROT_INVALID) {
        return BAD_VALUE;
    }
    tr->set(flags, w, h);
    return NO_ERROR;
}

void DisplayDevice::setDisplaySize(const int newWidth, const int newHeight) {
    mCompositionDisplay->setBounds(ui::Size(newWidth, newHeight));
}

void DisplayDevice::setProjection(int orientation,
        const Rect& newViewport, const Rect& newFrame) {
    Rect viewport(newViewport);
    Rect frame(newFrame);

    mOrientation = orientation;

    const Rect& displayBounds = getCompositionDisplay()->getState().bounds;
    const int w = displayBounds.width();
    const int h = displayBounds.height();

    R.reset();
    DisplayDevice::orientationToTransfrom(orientation, w, h, &R);

    if (!frame.isValid()) {
        // the destination frame can be invalid if it has never been set,
        // in that case we assume the whole display frame.
        frame = Rect(w, h);
    }

    if (viewport.isEmpty()) {
        // viewport can be invalid if it has never been set, in that case
        // we assume the whole display size.
        // it's also invalid to have an empty viewport, so we handle that
        // case in the same way.
        viewport = Rect(w, h);
        if (R.getOrientation() & ui::Transform::ROT_90) {
            // viewport is always specified in the logical orientation
            // of the display (ie: post-rotation).
            std::swap(viewport.right, viewport.bottom);
        }
    }

    TL.reset();
    TP.reset();
    S.reset();
    float src_width  = viewport.width();
    float src_height = viewport.height();
    float dst_width  = frame.width();
    float dst_height = frame.height();
    if (src_width != dst_width || src_height != dst_height) {
        float sx = dst_width  / src_width;
        float sy = dst_height / src_height;
        S.set(sx, 0, 0, sy);
    }

    float src_x = viewport.left;
    float src_y = viewport.top;
    float dst_x = frame.left;
    float dst_y = frame.top;
    TL.set(-src_x, -src_y);
    TP.set(dst_x, dst_y);

    ui::Transform translate;
    translate.set(translateX, translateY);

    // need to take care of primary display rotation for globalTransform
    // for case if the panel is not installed aligned with device orientation
    if (isPrimary()) {
        DisplayDevice::orientationToTransfrom(
                (orientation + mDisplayInstallOrientation) % (DisplayState::eOrientation270 + 1),
                w, h, &R);
    }

    // The viewport and frame are both in the logical orientation.
    // Apply the logical translation, scale to physical size, apply the
    // physical translation and finally rotate to the physical orientation.
    ui::Transform globalTransform = translate * R * TP * S * TL;

    const uint8_t type = globalTransform.getType();
    const bool needsFiltering =
            (!globalTransform.preserveRects() || (type >= ui::Transform::SCALE));

    Rect scissor = globalTransform.transform(viewport);
    if (scissor.isEmpty()) {
        scissor = displayBounds;
    }

    uint32_t transformOrientation;

    if (isPrimary()) {
        sPrimaryDisplayOrientation = displayStateOrientationToTransformOrientation(orientation);
#ifdef CONSOLE_MANAGER
        mConsoleManagerThread = new ConsoleManagerThread(mFlinger, mDisplayToken);
#endif        
        transformOrientation = displayStateOrientationToTransformOrientation(
                (orientation + mDisplayInstallOrientation) % (DisplayState::eOrientation270 + 1));
    } else {
        transformOrientation = displayStateOrientationToTransformOrientation(orientation);
    }

    getCompositionDisplay()->setProjection(globalTransform, transformOrientation,
                                           frame, viewport, scissor, needsFiltering);
}

uint32_t DisplayDevice::getPrimaryDisplayOrientationTransform() {
    return sPrimaryDisplayOrientation;
}

std::string DisplayDevice::getDebugName() const {
    const auto id = getId() ? to_string(*getId()) + ", " : std::string();
    return base::StringPrintf("DisplayDevice{%s%s%s\"%s\"}", id.c_str(),
                              isPrimary() ? "primary, " : "", isVirtual() ? "virtual, " : "",
                              mDisplayName.c_str());
}

void DisplayDevice::setTranslate(int x, int y) {
    translateX = x;
    translateY = y;
}

void DisplayDevice::dump(std::string& result) const {
    StringAppendF(&result, "+ %s\n", getDebugName().c_str());

    result.append("   ");
    StringAppendF(&result, "powerMode=%d, ", mPowerMode);
    StringAppendF(&result, "activeConfig=%d, ", mActiveConfig);
    StringAppendF(&result, "numLayers=%zu\n", mVisibleLayersSortedByZ.size());
    getCompositionDisplay()->dump(result);
}

bool DisplayDevice::hasRenderIntent(ui::RenderIntent intent) const {
    return mCompositionDisplay->getDisplayColorProfile()->hasRenderIntent(intent);
}

// ----------------------------------------------------------------------------

const std::optional<DisplayId>& DisplayDevice::getId() const {
    return mCompositionDisplay->getId();
}

bool DisplayDevice::isSecure() const {
    return mCompositionDisplay->isSecure();
}

const Rect& DisplayDevice::getBounds() const {
    return mCompositionDisplay->getState().bounds;
}

const Region& DisplayDevice::getUndefinedRegion() const {
    return mCompositionDisplay->getState().undefinedRegion;
}

bool DisplayDevice::needsFiltering() const {
    return mCompositionDisplay->getState().needsFiltering;
}

uint32_t DisplayDevice::getLayerStack() const {
    return mCompositionDisplay->getState().layerStackId;
}

const ui::Transform& DisplayDevice::getTransform() const {
    return mCompositionDisplay->getState().transform;
}

const Rect& DisplayDevice::getViewport() const {
    return mCompositionDisplay->getState().viewport;
}

const Rect& DisplayDevice::getFrame() const {
    return mCompositionDisplay->getState().frame;
}

const Rect& DisplayDevice::getScissor() const {
    return mCompositionDisplay->getState().scissor;
}

bool DisplayDevice::hasWideColorGamut() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasWideColorGamut();
}

bool DisplayDevice::hasHDR10PlusSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHDR10PlusSupport();
}

bool DisplayDevice::hasHDR10Support() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHDR10Support();
}

bool DisplayDevice::hasHLGSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHLGSupport();
}

bool DisplayDevice::hasDolbyVisionSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasDolbyVisionSupport();
}

int DisplayDevice::getSupportedPerFrameMetadata() const {
    return mCompositionDisplay->getDisplayColorProfile()->getSupportedPerFrameMetadata();
}

const HdrCapabilities& DisplayDevice::getHdrCapabilities() const {
    return mCompositionDisplay->getDisplayColorProfile()->getHdrCapabilities();
}

std::atomic<int32_t> DisplayDeviceState::sNextSequenceId(1);

}  // namespace android
