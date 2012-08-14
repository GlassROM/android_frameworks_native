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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <math.h>

#include <EGL/egl.h>
#include <GLES/gl.h>

#include <cutils/log.h>
#include <cutils/properties.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryHeapBase.h>
#include <binder/PermissionCache.h>

#include <ui/DisplayInfo.h>

#include <gui/BitTube.h>
#include <gui/BufferQueue.h>
#include <gui/IDisplayEventConnection.h>
#include <gui/SurfaceTextureClient.h>

#include <ui/GraphicBufferAllocator.h>
#include <ui/PixelFormat.h>

#include <utils/String8.h>
#include <utils/String16.h>
#include <utils/StopWatch.h>
#include <utils/Trace.h>

#include <private/android_filesystem_config.h>

#include "clz.h"
#include "DdmConnection.h"
#include "DisplayDevice.h"
#include "Client.h"
#include "EventThread.h"
#include "GLExtensions.h"
#include "Layer.h"
#include "LayerDim.h"
#include "LayerScreenshot.h"
#include "SurfaceFlinger.h"

#include "DisplayHardware/FramebufferSurface.h"
#include "DisplayHardware/HWComposer.h"


#define EGL_VERSION_HW_ANDROID  0x3143

#define DISPLAY_COUNT       1

namespace android {
// ---------------------------------------------------------------------------

const String16 sHardwareTest("android.permission.HARDWARE_TEST");
const String16 sAccessSurfaceFlinger("android.permission.ACCESS_SURFACE_FLINGER");
const String16 sReadFramebuffer("android.permission.READ_FRAME_BUFFER");
const String16 sDump("android.permission.DUMP");

// ---------------------------------------------------------------------------

SurfaceFlinger::SurfaceFlinger()
    :   BnSurfaceComposer(), Thread(false),
        mTransactionFlags(0),
        mTransationPending(false),
        mLayersRemoved(false),
        mRepaintEverything(0),
        mBootTime(systemTime()),
        mVisibleRegionsDirty(false),
        mHwWorkListDirty(false),
        mDebugRegion(0),
        mDebugDDMS(0),
        mDebugDisableHWC(0),
        mDebugDisableTransformHint(0),
        mDebugInSwapBuffers(0),
        mLastSwapBufferTime(0),
        mDebugInTransaction(0),
        mLastTransactionTime(0),
        mBootFinished(false),
        mExternalDisplaySurface(EGL_NO_SURFACE)
{
    ALOGI("SurfaceFlinger is starting");

    // debugging stuff...
    char value[PROPERTY_VALUE_MAX];

    property_get("debug.sf.showupdates", value, "0");
    mDebugRegion = atoi(value);

    property_get("debug.sf.ddms", value, "0");
    mDebugDDMS = atoi(value);
    if (mDebugDDMS) {
        DdmConnection::start(getServiceName());
    }

    ALOGI_IF(mDebugRegion, "showupdates enabled");
    ALOGI_IF(mDebugDDMS, "DDMS debugging enabled");
}

void SurfaceFlinger::onFirstRef()
{
    mEventQueue.init(this);

    run("SurfaceFlinger", PRIORITY_URGENT_DISPLAY);

    // Wait for the main thread to be done with its initialization
    mReadyToRunBarrier.wait();
}


SurfaceFlinger::~SurfaceFlinger()
{
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(display);
}

void SurfaceFlinger::binderDied(const wp<IBinder>& who)
{
    // the window manager died on us. prepare its eulogy.

    // reset screen orientation
    Vector<ComposerState> state;
    Vector<DisplayState> displays;
    DisplayState d;
    d.orientation = DisplayState::eOrientationDefault;
    displays.add(d);
    setTransactionState(state, displays, 0);

    // restart the boot-animation
    startBootAnim();
}

sp<ISurfaceComposerClient> SurfaceFlinger::createConnection()
{
    sp<ISurfaceComposerClient> bclient;
    sp<Client> client(new Client(this));
    status_t err = client->initCheck();
    if (err == NO_ERROR) {
        bclient = client;
    }
    return bclient;
}

sp<IBinder> SurfaceFlinger::createDisplay()
{
    class DisplayToken : public BBinder {
        sp<SurfaceFlinger> flinger;
        virtual ~DisplayToken() {
             // no more references, this display must be terminated
             Mutex::Autolock _l(flinger->mStateLock);
             flinger->mCurrentState.displays.removeItem(this);
             flinger->setTransactionFlags(eDisplayTransactionNeeded);
         }
     public:
        DisplayToken(const sp<SurfaceFlinger>& flinger)
            : flinger(flinger) {
        }
    };

    sp<BBinder> token = new DisplayToken(this);

    Mutex::Autolock _l(mStateLock);
    DisplayDeviceState info(intptr_t(token.get())); // FIXME: we shouldn't use the address for the id
    mCurrentState.displays.add(token, info);

    return token;
}

sp<IBinder> SurfaceFlinger::getBuiltInDisplay(int32_t id) {
    if (uint32_t(id) >= DisplayDevice::DISPLAY_ID_COUNT) {
        ALOGE("getDefaultDisplay: id=%d is not a valid default display id", id);
        return NULL;
    }
    return mDefaultDisplays[id];
}

sp<IGraphicBufferAlloc> SurfaceFlinger::createGraphicBufferAlloc()
{
    sp<GraphicBufferAlloc> gba(new GraphicBufferAlloc());
    return gba;
}

void SurfaceFlinger::bootFinished()
{
    const nsecs_t now = systemTime();
    const nsecs_t duration = now - mBootTime;
    ALOGI("Boot is finished (%ld ms)", long(ns2ms(duration)) );
    mBootFinished = true;

    // wait patiently for the window manager death
    const String16 name("window");
    sp<IBinder> window(defaultServiceManager()->getService(name));
    if (window != 0) {
        window->linkToDeath(static_cast<IBinder::DeathRecipient*>(this));
    }

    // stop boot animation
    // formerly we would just kill the process, but we now ask it to exit so it
    // can choose where to stop the animation.
    property_set("service.bootanim.exit", "1");
}

void SurfaceFlinger::deleteTextureAsync(GLuint texture) {
    class MessageDestroyGLTexture : public MessageBase {
        GLuint texture;
    public:
        MessageDestroyGLTexture(GLuint texture)
            : texture(texture) {
        }
        virtual bool handler() {
            glDeleteTextures(1, &texture);
            return true;
        }
    };
    postMessageAsync(new MessageDestroyGLTexture(texture));
}

status_t SurfaceFlinger::selectConfigForPixelFormat(
        EGLDisplay dpy,
        EGLint const* attrs,
        PixelFormat format,
        EGLConfig* outConfig)
{
    EGLConfig config = NULL;
    EGLint numConfigs = -1, n=0;
    eglGetConfigs(dpy, NULL, 0, &numConfigs);
    EGLConfig* const configs = new EGLConfig[numConfigs];
    eglChooseConfig(dpy, attrs, configs, numConfigs, &n);
    for (int i=0 ; i<n ; i++) {
        EGLint nativeVisualId = 0;
        eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &nativeVisualId);
        if (nativeVisualId>0 && format == nativeVisualId) {
            *outConfig = configs[i];
            delete [] configs;
            return NO_ERROR;
        }
    }
    delete [] configs;
    return NAME_NOT_FOUND;
}

EGLConfig SurfaceFlinger::selectEGLConfig(EGLDisplay display, EGLint nativeVisualId) {
    // select our EGLConfig. It must support EGL_RECORDABLE_ANDROID if
    // it is to be used with WIFI displays
    EGLConfig config;
    EGLint dummy;
    status_t err;
    EGLint attribs[] = {
            EGL_SURFACE_TYPE,           EGL_WINDOW_BIT,
            EGL_RECORDABLE_ANDROID,     EGL_TRUE,
            EGL_NONE
    };
    err = selectConfigForPixelFormat(display, attribs, nativeVisualId, &config);
    if (err) {
        // maybe we failed because of EGL_RECORDABLE_ANDROID
        ALOGW("couldn't find an EGLConfig with EGL_RECORDABLE_ANDROID");
        attribs[2] = EGL_NONE;
        err = selectConfigForPixelFormat(display, attribs, nativeVisualId, &config);
    }
    ALOGE_IF(err, "couldn't find an EGLConfig matching the screen format");
    if (eglGetConfigAttrib(display, config, EGL_CONFIG_CAVEAT, &dummy) == EGL_TRUE) {
        ALOGW_IF(dummy == EGL_SLOW_CONFIG, "EGL_SLOW_CONFIG selected!");
    }
    return config;
}

EGLContext SurfaceFlinger::createGLContext(EGLDisplay display, EGLConfig config) {
    // Also create our EGLContext
    EGLint contextAttributes[] = {
#ifdef EGL_IMG_context_priority
#ifdef HAS_CONTEXT_PRIORITY
#warning "using EGL_IMG_context_priority"
            EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG,
#endif
#endif
            EGL_NONE, EGL_NONE
    };
    EGLContext ctxt = eglCreateContext(display, config, NULL, contextAttributes);
    ALOGE_IF(ctxt==EGL_NO_CONTEXT, "EGLContext creation failed");
    return ctxt;
}

void SurfaceFlinger::initializeGL(EGLDisplay display, EGLSurface surface) {
    EGLBoolean result = eglMakeCurrent(display, surface, surface, mEGLContext);
    if (!result) {
        ALOGE("Couldn't create a working GLES context. check logs. exiting...");
        exit(0);
    }

    GLExtensions& extensions(GLExtensions::getInstance());
    extensions.initWithGLStrings(
            glGetString(GL_VENDOR),
            glGetString(GL_RENDERER),
            glGetString(GL_VERSION),
            glGetString(GL_EXTENSIONS),
            eglQueryString(display, EGL_VENDOR),
            eglQueryString(display, EGL_VERSION),
            eglQueryString(display, EGL_EXTENSIONS));

    EGLint w, h;
    eglQuerySurface(display, surface, EGL_WIDTH,  &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mMaxTextureSize);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, mMaxViewportDims);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glEnableClientState(GL_VERTEX_ARRAY);
    glShadeModel(GL_FLAT);
    glDisable(GL_DITHER);
    glDisable(GL_CULL_FACE);

    struct pack565 {
        inline uint16_t operator() (int r, int g, int b) const {
            return (r<<11)|(g<<5)|b;
        }
    } pack565;

    const uint16_t protTexData[] = { pack565(0x03, 0x03, 0x03) };
    glGenTextures(1, &mProtectedTexName);
    glBindTexture(GL_TEXTURE_2D, mProtectedTexName);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0,
            GL_RGB, GL_UNSIGNED_SHORT_5_6_5, protTexData);

    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // put the origin in the left-bottom corner
    glOrthof(0, w, 0, h, 0, 1); // l=0, r=w ; b=0, t=h

    // print some debugging info
    EGLint r,g,b,a;
    eglGetConfigAttrib(display, mEGLConfig, EGL_RED_SIZE,   &r);
    eglGetConfigAttrib(display, mEGLConfig, EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib(display, mEGLConfig, EGL_BLUE_SIZE,  &b);
    eglGetConfigAttrib(display, mEGLConfig, EGL_ALPHA_SIZE, &a);
    ALOGI("EGL informations:");
    ALOGI("vendor    : %s", extensions.getEglVendor());
    ALOGI("version   : %s", extensions.getEglVersion());
    ALOGI("extensions: %s", extensions.getEglExtension());
    ALOGI("Client API: %s", eglQueryString(display, EGL_CLIENT_APIS)?:"Not Supported");
    ALOGI("EGLSurface: %d-%d-%d-%d, config=%p", r, g, b, a, mEGLConfig);
    ALOGI("OpenGL ES informations:");
    ALOGI("vendor    : %s", extensions.getVendor());
    ALOGI("renderer  : %s", extensions.getRenderer());
    ALOGI("version   : %s", extensions.getVersion());
    ALOGI("extensions: %s", extensions.getExtension());
    ALOGI("GL_MAX_TEXTURE_SIZE = %d", mMaxTextureSize);
    ALOGI("GL_MAX_VIEWPORT_DIMS = %d x %d", mMaxViewportDims[0], mMaxViewportDims[1]);
}

status_t SurfaceFlinger::readyToRun()
{
    ALOGI(  "SurfaceFlinger's main thread ready to run. "
            "Initializing graphics H/W...");

    // initialize EGL
    mEGLDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(mEGLDisplay, NULL, NULL);

    // Initialize the main display
    // create native window to main display
    sp<FramebufferSurface> fbs = FramebufferSurface::create();
    if (fbs == NULL) {
        ALOGE("Display subsystem failed to initialize. check logs. exiting...");
        exit(0);
    }

    sp<SurfaceTextureClient> stc(new SurfaceTextureClient(
            static_cast<sp<ISurfaceTexture> >(fbs->getBufferQueue())));

    // initialize the config and context
    int format;
    ANativeWindow* const anw = stc.get();
    anw->query(anw, NATIVE_WINDOW_FORMAT, &format);
    mEGLConfig  = selectEGLConfig(mEGLDisplay, format);
    mEGLContext = createGLContext(mEGLDisplay, mEGLConfig);

    // initialize our main display hardware

    for (size_t i=0 ; i<DisplayDevice::DISPLAY_ID_COUNT ; i++) {
        mDefaultDisplays[i] = new BBinder();
        mCurrentState.displays.add(mDefaultDisplays[i], DisplayDeviceState(i));
    }
    sp<DisplayDevice> hw = new DisplayDevice(this,
            DisplayDevice::DISPLAY_ID_MAIN, anw, fbs, mEGLConfig);
    mDisplays.add(hw->getDisplayId(), hw);

    //  initialize OpenGL ES
    EGLSurface surface = hw->getEGLSurface();
    initializeGL(mEGLDisplay, surface);

    // start the EventThread
    mEventThread = new EventThread(this);
    mEventQueue.setEventThread(mEventThread);

    // initialize the H/W composer
    mHwc = new HWComposer(this,
            *static_cast<HWComposer::EventHandler *>(this),
            fbs->getFbHal());

    // initialize our drawing state
    mDrawingState = mCurrentState;

    // We're now ready to accept clients...
    mReadyToRunBarrier.open();

    // start boot animation
    startBootAnim();

    return NO_ERROR;
}

void SurfaceFlinger::startBootAnim() {
    // start boot animation
    property_set("service.bootanim.exit", "0");
    property_set("ctl.start", "bootanim");
}

uint32_t SurfaceFlinger::getMaxTextureSize() const {
    return mMaxTextureSize;
}

uint32_t SurfaceFlinger::getMaxViewportDims() const {
    return mMaxViewportDims[0] < mMaxViewportDims[1] ?
            mMaxViewportDims[0] : mMaxViewportDims[1];
}

// ----------------------------------------------------------------------------

bool SurfaceFlinger::authenticateSurfaceTexture(
        const sp<ISurfaceTexture>& surfaceTexture) const {
    Mutex::Autolock _l(mStateLock);
    sp<IBinder> surfaceTextureBinder(surfaceTexture->asBinder());

    // Check the visible layer list for the ISurface
    const LayerVector& currentLayers = mCurrentState.layersSortedByZ;
    size_t count = currentLayers.size();
    for (size_t i=0 ; i<count ; i++) {
        const sp<LayerBase>& layer(currentLayers[i]);
        sp<LayerBaseClient> lbc(layer->getLayerBaseClient());
        if (lbc != NULL) {
            wp<IBinder> lbcBinder = lbc->getSurfaceTextureBinder();
            if (lbcBinder == surfaceTextureBinder) {
                return true;
            }
        }
    }

    // Check the layers in the purgatory.  This check is here so that if a
    // SurfaceTexture gets destroyed before all the clients are done using it,
    // the error will not be reported as "surface XYZ is not authenticated", but
    // will instead fail later on when the client tries to use the surface,
    // which should be reported as "surface XYZ returned an -ENODEV".  The
    // purgatorized layers are no less authentic than the visible ones, so this
    // should not cause any harm.
    size_t purgatorySize =  mLayerPurgatory.size();
    for (size_t i=0 ; i<purgatorySize ; i++) {
        const sp<LayerBase>& layer(mLayerPurgatory.itemAt(i));
        sp<LayerBaseClient> lbc(layer->getLayerBaseClient());
        if (lbc != NULL) {
            wp<IBinder> lbcBinder = lbc->getSurfaceTextureBinder();
            if (lbcBinder == surfaceTextureBinder) {
                return true;
            }
        }
    }

    return false;
}

status_t SurfaceFlinger::getDisplayInfo(DisplayID dpy, DisplayInfo* info) {
    // TODO: this is here only for compatibility -- should go away eventually.
    if (uint32_t(dpy) >= 1) {
        return BAD_INDEX;
    }

    const HWComposer& hwc(getHwComposer());
    float xdpi = hwc.getDpiX();
    float ydpi = hwc.getDpiY();

    // TODO: Not sure if display density should handled by SF any longer
    class Density {
        static int getDensityFromProperty(char const* propName) {
            char property[PROPERTY_VALUE_MAX];
            int density = 0;
            if (property_get(propName, property, NULL) > 0) {
                density = atoi(property);
            }
            return density;
        }
    public:
        static int getEmuDensity() {
            return getDensityFromProperty("qemu.sf.lcd_density"); }
        static int getBuildDensity()  {
            return getDensityFromProperty("ro.sf.lcd_density"); }
    };
    // The density of the device is provided by a build property
    float density = Density::getBuildDensity() / 160.0f;
    if (density == 0) {
        // the build doesn't provide a density -- this is wrong!
        // use xdpi instead
        ALOGE("ro.sf.lcd_density must be defined as a build property");
        density = xdpi / 160.0f;
    }
    if (Density::getEmuDensity()) {
        // if "qemu.sf.lcd_density" is specified, it overrides everything
        xdpi = ydpi = density = Density::getEmuDensity();
        density /= 160.0f;
    }

    sp<const DisplayDevice> hw(getDefaultDisplayDevice());
    info->w = hw->getWidth();
    info->h = hw->getHeight();
    info->xdpi = xdpi;
    info->ydpi = ydpi;
    info->fps = float(1e9 / hwc.getRefreshPeriod());
    info->density = density;
    info->orientation = hw->getOrientation();
    // TODO: this needs to go away (currently needed only by webkit)
    getPixelFormatInfo(hw->getFormat(), &info->pixelFormatInfo);
    return NO_ERROR;
}

// ----------------------------------------------------------------------------

sp<IDisplayEventConnection> SurfaceFlinger::createDisplayEventConnection() {
    return mEventThread->createEventConnection();
}

void SurfaceFlinger::connectDisplay(const sp<ISurfaceTexture> display) {
    EGLSurface result = EGL_NO_SURFACE;
    EGLSurface old_surface = EGL_NO_SURFACE;
    sp<SurfaceTextureClient> stc;

    if (display != NULL) {
        stc = new SurfaceTextureClient(display);
        result = eglCreateWindowSurface(mEGLDisplay,
                mEGLConfig, (EGLNativeWindowType)stc.get(), NULL);
        ALOGE_IF(result == EGL_NO_SURFACE,
                "eglCreateWindowSurface failed (ISurfaceTexture=%p)",
                display.get());
    }

    { // scope for the lock
        Mutex::Autolock _l(mStateLock);
        old_surface = mExternalDisplaySurface;
        mExternalDisplayNativeWindow = stc;
        mExternalDisplaySurface = result;
        ALOGD("mExternalDisplaySurface = %p", result);
    }

    if (old_surface != EGL_NO_SURFACE) {
        // Note: EGL allows to destroy an object while its current
        // it will fail to become current next time though.
        eglDestroySurface(mEGLDisplay, old_surface);
    }
}

EGLSurface SurfaceFlinger::getExternalDisplaySurface() const {
    Mutex::Autolock _l(mStateLock);
    return mExternalDisplaySurface;
}

// ----------------------------------------------------------------------------

void SurfaceFlinger::waitForEvent() {
    mEventQueue.waitMessage();
}

void SurfaceFlinger::signalTransaction() {
    mEventQueue.invalidate();
}

void SurfaceFlinger::signalLayerUpdate() {
    mEventQueue.invalidate();
}

void SurfaceFlinger::signalRefresh() {
    mEventQueue.refresh();
}

status_t SurfaceFlinger::postMessageAsync(const sp<MessageBase>& msg,
        nsecs_t reltime, uint32_t flags) {
    return mEventQueue.postMessage(msg, reltime);
}

status_t SurfaceFlinger::postMessageSync(const sp<MessageBase>& msg,
        nsecs_t reltime, uint32_t flags) {
    status_t res = mEventQueue.postMessage(msg, reltime);
    if (res == NO_ERROR) {
        msg->wait();
    }
    return res;
}

bool SurfaceFlinger::threadLoop() {
    waitForEvent();
    return true;
}

void SurfaceFlinger::onVSyncReceived(int dpy, nsecs_t timestamp) {
    mEventThread->onVSyncReceived(dpy, timestamp);
}

void SurfaceFlinger::eventControl(int event, int enabled) {
    getHwComposer().eventControl(event, enabled);
}

void SurfaceFlinger::onMessageReceived(int32_t what) {
    ATRACE_CALL();
    switch (what) {
    case MessageQueue::INVALIDATE:
        handleMessageTransaction();
        handleMessageInvalidate();
        signalRefresh();
        break;
    case MessageQueue::REFRESH:
        handleMessageRefresh();
        break;
    }
}

void SurfaceFlinger::handleMessageTransaction() {
    uint32_t transactionFlags = peekTransactionFlags(eTransactionMask);
    if (transactionFlags) {
        handleTransaction(transactionFlags);
    }
}

void SurfaceFlinger::handleMessageInvalidate() {
    handlePageFlip();
}

void SurfaceFlinger::handleMessageRefresh() {
    handleRefresh();

    if (CC_UNLIKELY(mVisibleRegionsDirty)) {
        mVisibleRegionsDirty = false;
        invalidateHwcGeometry();

        /*
         *  rebuild the visible layer list per screen
         */

        const LayerVector& currentLayers(mDrawingState.layersSortedByZ);
        for (size_t dpy=0 ; dpy<mDisplays.size() ; dpy++) {
            const sp<DisplayDevice>& hw(mDisplays[dpy]);
            Region opaqueRegion;
            Region dirtyRegion;
            computeVisibleRegions(currentLayers,
                    hw->getLayerStack(), dirtyRegion, opaqueRegion);
            hw->dirtyRegion.orSelf(dirtyRegion);

            Vector< sp<LayerBase> > layersSortedByZ;
            const size_t count = currentLayers.size();
            for (size_t i=0 ; i<count ; i++) {
                const Layer::State& s(currentLayers[i]->drawingState());
                if (s.layerStack == hw->getLayerStack()) {
                    if (!currentLayers[i]->visibleRegion.isEmpty()) {
                        layersSortedByZ.add(currentLayers[i]);
                    }
                }
            }
            hw->setVisibleLayersSortedByZ(layersSortedByZ);
            hw->undefinedRegion.set(hw->getBounds());
            hw->undefinedRegion.subtractSelf(hw->getTransform().transform(opaqueRegion));
        }
    }

    HWComposer& hwc(getHwComposer());
    if (hwc.initCheck() == NO_ERROR) {
        // build the h/w work list
        const bool workListsDirty = mHwWorkListDirty;
        mHwWorkListDirty = false;
        for (size_t dpy=0 ; dpy<mDisplays.size() ; dpy++) {
            sp<const DisplayDevice> hw(mDisplays[dpy]);
            const Vector< sp<LayerBase> >& currentLayers(hw->getVisibleLayersSortedByZ());
            const size_t count = currentLayers.size();

            const int32_t id = hw->getDisplayId();
            if (hwc.createWorkList(id, count) >= 0) {
                HWComposer::LayerListIterator cur = hwc.begin(id);
                const HWComposer::LayerListIterator end = hwc.end(id);
                for (size_t i=0 ; cur!=end && i<count ; ++i, ++cur) {
                    const sp<LayerBase>& layer(currentLayers[i]);

                    if (CC_UNLIKELY(workListsDirty)) {
                        layer->setGeometry(hw, *cur);
                        if (mDebugDisableHWC || mDebugRegion) {
                            cur->setSkip(true);
                        }
                    }

                    /*
                     * update the per-frame h/w composer data for each layer
                     * and build the transparent region of the FB
                     */
                    layer->setPerFrameData(hw, *cur);
                }
            }
        }
        status_t err = hwc.prepare();
        ALOGE_IF(err, "HWComposer::prepare failed (%s)", strerror(-err));
    }

    const bool repaintEverything = android_atomic_and(0, &mRepaintEverything);
    for (size_t dpy=0 ; dpy<mDisplays.size() ; dpy++) {
        const sp<DisplayDevice>& hw(mDisplays[dpy]);

        // transform the dirty region into this screen's coordinate space
        const Transform& planeTransform(hw->getTransform());
        Region dirtyRegion;
        if (repaintEverything) {
            dirtyRegion.set(hw->bounds());
        } else {
            dirtyRegion = planeTransform.transform(hw->dirtyRegion);
            dirtyRegion.andSelf(hw->bounds());
        }
        hw->dirtyRegion.clear();

        if (!dirtyRegion.isEmpty()) {
            if (hw->canDraw()) {
                // repaint the framebuffer (if needed)
                handleRepaint(hw, dirtyRegion);
            }
        }
        // inform the h/w that we're done compositing
        hw->compositionComplete();
    }

    postFramebuffer();


#if 1
    // render to the external display if we have one
    EGLSurface externalDisplaySurface = getExternalDisplaySurface();
    if (externalDisplaySurface != EGL_NO_SURFACE) {
        EGLSurface cur = eglGetCurrentSurface(EGL_DRAW);
        EGLBoolean success = eglMakeCurrent(eglGetCurrentDisplay(),
                externalDisplaySurface, externalDisplaySurface,
                eglGetCurrentContext());

        ALOGE_IF(!success, "eglMakeCurrent -> external failed");

        if (success) {
            // redraw the screen entirely...
            glDisable(GL_TEXTURE_EXTERNAL_OES);
            glDisable(GL_TEXTURE_2D);
            glClearColor(0,0,0,1);
            glClear(GL_COLOR_BUFFER_BIT);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();

            const sp<DisplayDevice>& hw(getDisplayDevice(0));
            const Vector< sp<LayerBase> >& layers( hw->getVisibleLayersSortedByZ() );
            const size_t count = layers.size();
            for (size_t i=0 ; i<count ; ++i) {
                const sp<LayerBase>& layer(layers[i]);
                layer->draw(hw);
            }

            success = eglSwapBuffers(eglGetCurrentDisplay(), externalDisplaySurface);
            ALOGE_IF(!success, "external display eglSwapBuffers failed");

            hw->compositionComplete();
        }

        success = eglMakeCurrent(eglGetCurrentDisplay(),
                cur, cur, eglGetCurrentContext());

        ALOGE_IF(!success, "eglMakeCurrent -> internal failed");
    }
#endif

}

void SurfaceFlinger::postFramebuffer()
{
    ATRACE_CALL();

    const nsecs_t now = systemTime();
    mDebugInSwapBuffers = now;

    HWComposer& hwc(getHwComposer());

    for (size_t dpy=0 ; dpy<mDisplays.size() ; dpy++) {
        const sp<DisplayDevice>& hw(mDisplays[dpy]);
        if (hwc.initCheck() == NO_ERROR) {
            const Vector< sp<LayerBase> >& currentLayers(hw->getVisibleLayersSortedByZ());
            const size_t count = currentLayers.size();
            const int32_t id = hw->getDisplayId();
            HWComposer::LayerListIterator cur = hwc.begin(id);
            const HWComposer::LayerListIterator end = hwc.end(id);
            for (size_t i=0 ; cur!=end && i<count ; ++i, ++cur) {
                const sp<LayerBase>& layer(currentLayers[i]);
                layer->setAcquireFence(hw, *cur);
            }
        }
        hw->flip(hw->swapRegion);
        hw->swapRegion.clear();
    }

    if (hwc.initCheck() == NO_ERROR) {
        // FIXME: eventually commit() won't take arguments
        hwc.commit(mEGLDisplay, getDefaultDisplayDevice()->getEGLSurface());
    }

    for (size_t dpy=0 ; dpy<mDisplays.size() ; dpy++) {
        sp<const DisplayDevice> hw(mDisplays[dpy]);
        const Vector< sp<LayerBase> >& currentLayers(hw->getVisibleLayersSortedByZ());
        const size_t count = currentLayers.size();
        if (hwc.initCheck() == NO_ERROR) {
            int32_t id = hw->getDisplayId();
            HWComposer::LayerListIterator cur = hwc.begin(id);
            const HWComposer::LayerListIterator end = hwc.end(id);
            for (size_t i = 0; cur != end && i < count; ++i, ++cur) {
                currentLayers[i]->onLayerDisplayed(hw, &*cur);
            }
        } else {
            eglSwapBuffers(mEGLDisplay, hw->getEGLSurface());
            for (size_t i = 0; i < count; i++) {
                currentLayers[i]->onLayerDisplayed(hw, NULL);
            }
        }

        // FIXME: we need to call eglSwapBuffers() on displays that have GL composition
    }

    mLastSwapBufferTime = systemTime() - now;
    mDebugInSwapBuffers = 0;
}

void SurfaceFlinger::handleTransaction(uint32_t transactionFlags)
{
    ATRACE_CALL();

    Mutex::Autolock _l(mStateLock);
    const nsecs_t now = systemTime();
    mDebugInTransaction = now;

    // Here we're guaranteed that some transaction flags are set
    // so we can call handleTransactionLocked() unconditionally.
    // We call getTransactionFlags(), which will also clear the flags,
    // with mStateLock held to guarantee that mCurrentState won't change
    // until the transaction is committed.

    transactionFlags = getTransactionFlags(eTransactionMask);
    handleTransactionLocked(transactionFlags);

    mLastTransactionTime = systemTime() - now;
    mDebugInTransaction = 0;
    invalidateHwcGeometry();
    // here the transaction has been committed
}

void SurfaceFlinger::handleTransactionLocked(uint32_t transactionFlags)
{
    const LayerVector& currentLayers(mCurrentState.layersSortedByZ);
    const size_t count = currentLayers.size();

    /*
     * Traversal of the children
     * (perform the transaction for each of them if needed)
     */

    const bool layersNeedTransaction = transactionFlags & eTraversalNeeded;
    if (layersNeedTransaction) {
        for (size_t i=0 ; i<count ; i++) {
            const sp<LayerBase>& layer = currentLayers[i];
            uint32_t trFlags = layer->getTransactionFlags(eTransactionNeeded);
            if (!trFlags) continue;

            const uint32_t flags = layer->doTransaction(0);
            if (flags & Layer::eVisibleRegion)
                mVisibleRegionsDirty = true;
        }
    }

    /*
     * Perform our own transaction if needed
     */

    if (transactionFlags & eDisplayTransactionNeeded) {
        // here we take advantage of Vector's copy-on-write semantics to
        // improve performance by skipping the transaction entirely when
        // know that the lists are identical
        const KeyedVector<  wp<IBinder>, DisplayDeviceState>& curr(mCurrentState.displays);
        const KeyedVector<  wp<IBinder>, DisplayDeviceState>& draw(mDrawingState.displays);
        if (!curr.isIdenticalTo(draw)) {
            mVisibleRegionsDirty = true;
            const size_t cc = curr.size();
            const size_t dc = draw.size();

            // find the displays that were removed
            // (ie: in drawing state but not in current state)
            // also handle displays that changed
            // (ie: displays that are in both lists)
            for (size_t i=0 ; i<dc ; i++) {
                const ssize_t j = curr.indexOfKey(draw.keyAt(i));
                if (j < 0) {
                    // in drawing state but not in current state
                    if (draw[i].id != DisplayDevice::DISPLAY_ID_MAIN) {
                        mDisplays.removeItem(draw[i].id);
                    } else {
                        ALOGW("trying to remove the main display");
                    }
                } else {
                    // this display is in both lists. see if something changed.
                    const DisplayDeviceState& state(curr[j]);
                    if (state.surface != draw[i].surface) {
                        // changing the surface is like destroying and
                        // recreating the DisplayDevice

                        sp<SurfaceTextureClient> stc(
                                new SurfaceTextureClient(state.surface));

                        sp<DisplayDevice> disp = new DisplayDevice(this,
                                state.id, stc, 0, mEGLConfig);

                        disp->setLayerStack(state.layerStack);
                        disp->setOrientation(state.orientation);
                        // TODO: take viewport and frame into account
                        mDisplays.replaceValueFor(state.id, disp);
                    }
                    if (state.layerStack != draw[i].layerStack) {
                        const sp<DisplayDevice>& disp(getDisplayDevice(state.id));
                        disp->setLayerStack(state.layerStack);
                    }
                    if (state.orientation != draw[i].orientation ||
                        state.viewport != draw[i].viewport ||
                        state.frame != draw[i].frame) {
                        const sp<DisplayDevice>& disp(getDisplayDevice(state.id));
                        disp->setOrientation(state.orientation);
                        // TODO: take viewport and frame into account
                    }
                }
            }

            // find displays that were added
            // (ie: in current state but not in drawing state)
            for (size_t i=0 ; i<cc ; i++) {
                if (draw.indexOfKey(curr.keyAt(i)) < 0) {
                    // FIXME: we need to pass the surface here
                    const DisplayDeviceState& state(curr[i]);
                    sp<SurfaceTextureClient> stc(
                            new SurfaceTextureClient(state.surface));
                    sp<DisplayDevice> disp = new DisplayDevice(this, state.id,
                            stc, 0, mEGLConfig);
                    mDisplays.add(state.id, disp);
                }
            }
        }

        if (currentLayers.size() > mDrawingState.layersSortedByZ.size()) {
            // layers have been added
            mVisibleRegionsDirty = true;
        }

        // some layers might have been removed, so
        // we need to update the regions they're exposing.
        if (mLayersRemoved) {
            mLayersRemoved = false;
            mVisibleRegionsDirty = true;
            const LayerVector& previousLayers(mDrawingState.layersSortedByZ);
            const size_t count = previousLayers.size();
            for (size_t i=0 ; i<count ; i++) {
                const sp<LayerBase>& layer(previousLayers[i]);
                if (currentLayers.indexOf(layer) < 0) {
                    // this layer is not visible anymore
                    // TODO: we could traverse the tree from front to back and
                    //       compute the actual visible region
                    // TODO: we could cache the transformed region
                    Layer::State front(layer->drawingState());
                    Region visibleReg = front.transform.transform(
                            Region(Rect(front.active.w, front.active.h)));
                    invalidateLayerStack(front.layerStack, visibleReg);
                }
            }
        }
    }

    commitTransaction();
}

void SurfaceFlinger::commitTransaction()
{
    if (!mLayersPendingRemoval.isEmpty()) {
        // Notify removed layers now that they can't be drawn from
        for (size_t i = 0; i < mLayersPendingRemoval.size(); i++) {
            mLayersPendingRemoval[i]->onRemoved();
        }
        mLayersPendingRemoval.clear();
    }

    mDrawingState = mCurrentState;
    mTransationPending = false;
    mTransactionCV.broadcast();
}

void SurfaceFlinger::computeVisibleRegions(
        const LayerVector& currentLayers, uint32_t layerStack,
        Region& outDirtyRegion, Region& outOpaqueRegion)
{
    ATRACE_CALL();

    Region aboveOpaqueLayers;
    Region aboveCoveredLayers;
    Region dirty;

    outDirtyRegion.clear();

    size_t i = currentLayers.size();
    while (i--) {
        const sp<LayerBase>& layer = currentLayers[i];

        // start with the whole surface at its current location
        const Layer::State& s(layer->drawingState());

        // only consider the layers on the given later stack
        if (s.layerStack != layerStack)
            continue;

        /*
         * opaqueRegion: area of a surface that is fully opaque.
         */
        Region opaqueRegion;

        /*
         * visibleRegion: area of a surface that is visible on screen
         * and not fully transparent. This is essentially the layer's
         * footprint minus the opaque regions above it.
         * Areas covered by a translucent surface are considered visible.
         */
        Region visibleRegion;

        /*
         * coveredRegion: area of a surface that is covered by all
         * visible regions above it (which includes the translucent areas).
         */
        Region coveredRegion;


        // handle hidden surfaces by setting the visible region to empty
        if (CC_LIKELY(!(s.flags & layer_state_t::eLayerHidden) && s.alpha)) {
            const bool translucent = !layer->isOpaque();
            Rect bounds(layer->computeBounds());
            visibleRegion.set(bounds);
            if (!visibleRegion.isEmpty()) {
                // Remove the transparent area from the visible region
                if (translucent) {
                    Region transparentRegionScreen;
                    const Transform tr(s.transform);
                    if (tr.transformed()) {
                        if (tr.preserveRects()) {
                            // transform the transparent region
                            transparentRegionScreen = tr.transform(s.transparentRegion);
                        } else {
                            // transformation too complex, can't do the
                            // transparent region optimization.
                            transparentRegionScreen.clear();
                        }
                    } else {
                        transparentRegionScreen = s.transparentRegion;
                    }
                    visibleRegion.subtractSelf(transparentRegionScreen);
                }

                // compute the opaque region
                const int32_t layerOrientation = s.transform.getOrientation();
                if (s.alpha==255 && !translucent &&
                        ((layerOrientation & Transform::ROT_INVALID) == false)) {
                    // the opaque region is the layer's footprint
                    opaqueRegion = visibleRegion;
                }
            }
        }

        // Clip the covered region to the visible region
        coveredRegion = aboveCoveredLayers.intersect(visibleRegion);

        // Update aboveCoveredLayers for next (lower) layer
        aboveCoveredLayers.orSelf(visibleRegion);

        // subtract the opaque region covered by the layers above us
        visibleRegion.subtractSelf(aboveOpaqueLayers);

        // compute this layer's dirty region
        if (layer->contentDirty) {
            // we need to invalidate the whole region
            dirty = visibleRegion;
            // as well, as the old visible region
            dirty.orSelf(layer->visibleRegion);
            layer->contentDirty = false;
        } else {
            /* compute the exposed region:
             *   the exposed region consists of two components:
             *   1) what's VISIBLE now and was COVERED before
             *   2) what's EXPOSED now less what was EXPOSED before
             *
             * note that (1) is conservative, we start with the whole
             * visible region but only keep what used to be covered by
             * something -- which mean it may have been exposed.
             *
             * (2) handles areas that were not covered by anything but got
             * exposed because of a resize.
             */
            const Region newExposed = visibleRegion - coveredRegion;
            const Region oldVisibleRegion = layer->visibleRegion;
            const Region oldCoveredRegion = layer->coveredRegion;
            const Region oldExposed = oldVisibleRegion - oldCoveredRegion;
            dirty = (visibleRegion&oldCoveredRegion) | (newExposed-oldExposed);
        }
        dirty.subtractSelf(aboveOpaqueLayers);

        // accumulate to the screen dirty region
        outDirtyRegion.orSelf(dirty);

        // Update aboveOpaqueLayers for next (lower) layer
        aboveOpaqueLayers.orSelf(opaqueRegion);

        // Store the visible region is screen space
        layer->setVisibleRegion(visibleRegion);
        layer->setCoveredRegion(coveredRegion);
    }

    outOpaqueRegion = aboveOpaqueLayers;
}

void SurfaceFlinger::invalidateLayerStack(uint32_t layerStack,
        const Region& dirty) {
    for (size_t dpy=0 ; dpy<mDisplays.size() ; dpy++) {
        const sp<DisplayDevice>& hw(mDisplays[dpy]);
        if (hw->getLayerStack() == layerStack) {
            hw->dirtyRegion.orSelf(dirty);
        }
    }
}

void SurfaceFlinger::handlePageFlip()
{
    ATRACE_CALL();
    Region dirtyRegion;

    const LayerVector& currentLayers(mDrawingState.layersSortedByZ);

    bool visibleRegions = false;
    const size_t count = currentLayers.size();
    sp<LayerBase> const* layers = currentLayers.array();
    for (size_t i=0 ; i<count ; i++) {
        const sp<LayerBase>& layer(layers[i]);
        const Region dirty(layer->latchBuffer(visibleRegions));
        Layer::State s(layer->drawingState());
        invalidateLayerStack(s.layerStack, dirty);
    }

    mVisibleRegionsDirty |= visibleRegions;
}

void SurfaceFlinger::invalidateHwcGeometry()
{
    mHwWorkListDirty = true;
}

void SurfaceFlinger::handleRefresh()
{
    bool needInvalidate = false;
    const LayerVector& currentLayers(mDrawingState.layersSortedByZ);
    const size_t count = currentLayers.size();
    for (size_t i=0 ; i<count ; i++) {
        const sp<LayerBase>& layer(currentLayers[i]);
        if (layer->onPreComposition()) {
            needInvalidate = true;
        }
    }
    if (needInvalidate) {
        signalLayerUpdate();
    }
}

void SurfaceFlinger::handleRepaint(const sp<const DisplayDevice>& hw,
        const Region& inDirtyRegion)
{
    ATRACE_CALL();

    Region dirtyRegion(inDirtyRegion);

    // compute the invalid region
    hw->swapRegion.orSelf(dirtyRegion);

    if (CC_UNLIKELY(mDebugRegion)) {
        debugFlashRegions(hw, dirtyRegion);
    }

    uint32_t flags = hw->getFlags();
    if (flags & DisplayDevice::SWAP_RECTANGLE) {
        // we can redraw only what's dirty, but since SWAP_RECTANGLE only
        // takes a rectangle, we must make sure to update that whole
        // rectangle in that case
        dirtyRegion.set(hw->swapRegion.bounds());
    } else {
        if (flags & DisplayDevice::PARTIAL_UPDATES) {
            // We need to redraw the rectangle that will be updated
            // (pushed to the framebuffer).
            // This is needed because PARTIAL_UPDATES only takes one
            // rectangle instead of a region (see DisplayDevice::flip())
            dirtyRegion.set(hw->swapRegion.bounds());
        } else {
            // we need to redraw everything (the whole screen)
            dirtyRegion.set(hw->bounds());
            hw->swapRegion = dirtyRegion;
        }
    }

    composeSurfaces(hw, dirtyRegion);

    const LayerVector& currentLayers(mDrawingState.layersSortedByZ);
    const size_t count = currentLayers.size();
    for (size_t i=0 ; i<count ; i++) {
        currentLayers[i]->onPostComposition();
    }

    // update the swap region and clear the dirty region
    hw->swapRegion.orSelf(dirtyRegion);
}

void SurfaceFlinger::composeSurfaces(const sp<const DisplayDevice>& hw, const Region& dirty)
{
    HWComposer& hwc(getHwComposer());
    int32_t id = hw->getDisplayId();
    HWComposer::LayerListIterator cur = hwc.begin(id);
    const HWComposer::LayerListIterator end = hwc.end(id);

    const size_t fbLayerCount = hwc.getLayerCount(id, HWC_FRAMEBUFFER);
    if (cur==end || fbLayerCount) {

        DisplayDevice::makeCurrent(hw, mEGLContext);

        // set the frame buffer
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // Never touch the framebuffer if we don't have any framebuffer layers
        if (hwc.getLayerCount(id, HWC_OVERLAY)) {
            // when using overlays, we assume a fully transparent framebuffer
            // NOTE: we could reduce how much we need to clear, for instance
            // remove where there are opaque FB layers. however, on some
            // GPUs doing a "clean slate" glClear might be more efficient.
            // We'll revisit later if needed.
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            const Region region(hw->undefinedRegion.intersect(dirty));
            // screen is already cleared here
            if (!region.isEmpty()) {
                // can happen with SurfaceView
                drawWormhole(region);
            }
        }

        /*
         * and then, render the layers targeted at the framebuffer
         */

        const Vector< sp<LayerBase> >& layers(hw->getVisibleLayersSortedByZ());
        const size_t count = layers.size();
        const Transform& tr = hw->getTransform();
        for (size_t i=0 ; i<count ; ++i) {
            const sp<LayerBase>& layer(layers[i]);
            const Region clip(dirty.intersect(tr.transform(layer->visibleRegion)));
            if (!clip.isEmpty()) {
                if (cur != end && cur->getCompositionType() == HWC_OVERLAY) {
                    if (i && (cur->getHints() & HWC_HINT_CLEAR_FB)
                            && layer->isOpaque()) {
                        // never clear the very first layer since we're
                        // guaranteed the FB is already cleared
                        layer->clearWithOpenGL(hw, clip);
                    }
                    ++cur;
                    continue;
                }
                // render the layer
                layer->draw(hw, clip);
            }
            if (cur != end) {
                ++cur;
            }
        }
    }
}

void SurfaceFlinger::debugFlashRegions(const sp<const DisplayDevice>& hw,
        const Region& dirtyRegion)
{
    const uint32_t flags = hw->getFlags();
    const int32_t height = hw->getHeight();
    if (hw->swapRegion.isEmpty()) {
        return;
    }

    if (!(flags & DisplayDevice::SWAP_RECTANGLE)) {
        const Region repaint((flags & DisplayDevice::PARTIAL_UPDATES) ?
                dirtyRegion.bounds() : hw->bounds());
        composeSurfaces(hw, repaint);
    }

    glDisable(GL_TEXTURE_EXTERNAL_OES);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);

    static int toggle = 0;
    toggle = 1 - toggle;
    if (toggle) {
        glColor4f(1, 0, 1, 1);
    } else {
        glColor4f(1, 1, 0, 1);
    }

    Region::const_iterator it = dirtyRegion.begin();
    Region::const_iterator const end = dirtyRegion.end();
    while (it != end) {
        const Rect& r = *it++;
        GLfloat vertices[][2] = {
                { r.left,  height - r.top },
                { r.left,  height - r.bottom },
                { r.right, height - r.bottom },
                { r.right, height - r.top }
        };
        glVertexPointer(2, GL_FLOAT, 0, vertices);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }

    hw->flip(hw->swapRegion);

    if (mDebugRegion > 1)
        usleep(mDebugRegion * 1000);
}

void SurfaceFlinger::drawWormhole(const Region& region) const
{
    glDisable(GL_TEXTURE_EXTERNAL_OES);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glColor4f(0,0,0,0);

    GLfloat vertices[4][2];
    glVertexPointer(2, GL_FLOAT, 0, vertices);
    Region::const_iterator it = region.begin();
    Region::const_iterator const end = region.end();
    while (it != end) {
        const Rect& r = *it++;
        vertices[0][0] = r.left;
        vertices[0][1] = r.top;
        vertices[1][0] = r.right;
        vertices[1][1] = r.top;
        vertices[2][0] = r.right;
        vertices[2][1] = r.bottom;
        vertices[3][0] = r.left;
        vertices[3][1] = r.bottom;
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
}

ssize_t SurfaceFlinger::addClientLayer(const sp<Client>& client,
        const sp<LayerBaseClient>& lbc)
{
    // attach this layer to the client
    size_t name = client->attachLayer(lbc);

    // add this layer to the current state list
    Mutex::Autolock _l(mStateLock);
    mCurrentState.layersSortedByZ.add(lbc);

    return ssize_t(name);
}

status_t SurfaceFlinger::removeLayer(const sp<LayerBase>& layer)
{
    Mutex::Autolock _l(mStateLock);
    status_t err = purgatorizeLayer_l(layer);
    if (err == NO_ERROR)
        setTransactionFlags(eTransactionNeeded);
    return err;
}

status_t SurfaceFlinger::removeLayer_l(const sp<LayerBase>& layerBase)
{
    ssize_t index = mCurrentState.layersSortedByZ.remove(layerBase);
    if (index >= 0) {
        mLayersRemoved = true;
        return NO_ERROR;
    }
    return status_t(index);
}

status_t SurfaceFlinger::purgatorizeLayer_l(const sp<LayerBase>& layerBase)
{
    // First add the layer to the purgatory list, which makes sure it won't
    // go away, then remove it from the main list (through a transaction).
    ssize_t err = removeLayer_l(layerBase);
    if (err >= 0) {
        mLayerPurgatory.add(layerBase);
    }

    mLayersPendingRemoval.push(layerBase);

    // it's possible that we don't find a layer, because it might
    // have been destroyed already -- this is not technically an error
    // from the user because there is a race between Client::destroySurface(),
    // ~Client() and ~ISurface().
    return (err == NAME_NOT_FOUND) ? status_t(NO_ERROR) : err;
}

uint32_t SurfaceFlinger::peekTransactionFlags(uint32_t flags)
{
    return android_atomic_release_load(&mTransactionFlags);
}

uint32_t SurfaceFlinger::getTransactionFlags(uint32_t flags)
{
    return android_atomic_and(~flags, &mTransactionFlags) & flags;
}

uint32_t SurfaceFlinger::setTransactionFlags(uint32_t flags)
{
    uint32_t old = android_atomic_or(flags, &mTransactionFlags);
    if ((old & flags)==0) { // wake the server up
        signalTransaction();
    }
    return old;
}

void SurfaceFlinger::setTransactionState(
        const Vector<ComposerState>& state,
        const Vector<DisplayState>& displays,
        uint32_t flags)
{
    Mutex::Autolock _l(mStateLock);
    uint32_t transactionFlags = 0;

    size_t count = displays.size();
    for (size_t i=0 ; i<count ; i++) {
        const DisplayState& s(displays[i]);
        transactionFlags |= setDisplayStateLocked(s);
    }

    count = state.size();
    for (size_t i=0 ; i<count ; i++) {
        const ComposerState& s(state[i]);
        sp<Client> client( static_cast<Client *>(s.client.get()) );
        transactionFlags |= setClientStateLocked(client, s.state);
    }

    if (transactionFlags) {
        // this triggers the transaction
        setTransactionFlags(transactionFlags);

        // if this is a synchronous transaction, wait for it to take effect
        // before returning.
        if (flags & eSynchronous) {
            mTransationPending = true;
        }
        while (mTransationPending) {
            status_t err = mTransactionCV.waitRelative(mStateLock, s2ns(5));
            if (CC_UNLIKELY(err != NO_ERROR)) {
                // just in case something goes wrong in SF, return to the
                // called after a few seconds.
                ALOGW_IF(err == TIMED_OUT, "closeGlobalTransaction timed out!");
                mTransationPending = false;
                break;
            }
        }
    }
}

uint32_t SurfaceFlinger::setDisplayStateLocked(const DisplayState& s)
{
    uint32_t flags = 0;
    DisplayDeviceState& disp(mCurrentState.displays.editValueFor(s.token));
    if (disp.id >= 0) {
        const uint32_t what = s.what;
        if (what & DisplayState::eSurfaceChanged) {
            if (disp.surface->asBinder() != s.surface->asBinder()) {
                disp.surface = s.surface;
                flags |= eDisplayTransactionNeeded;
            }
        }
        if (what & DisplayState::eLayerStackChanged) {
            if (disp.layerStack != s.layerStack) {
                disp.layerStack = s.layerStack;
                flags |= eDisplayTransactionNeeded;
            }
        }
        if (what & DisplayState::eTransformChanged) {
            if (disp.orientation != s.orientation) {
                disp.orientation = s.orientation;
                flags |= eDisplayTransactionNeeded;
            }
            if (disp.frame != s.frame) {
                disp.frame = s.frame;
                flags |= eDisplayTransactionNeeded;
            }
            if (disp.viewport != s.viewport) {
                disp.viewport = s.viewport;
                flags |= eDisplayTransactionNeeded;
            }
        }
    }
    return flags;
}

uint32_t SurfaceFlinger::setClientStateLocked(
        const sp<Client>& client,
        const layer_state_t& s)
{
    uint32_t flags = 0;
    sp<LayerBaseClient> layer(client->getLayerUser(s.surface));
    if (layer != 0) {
        const uint32_t what = s.what;
        if (what & layer_state_t::ePositionChanged) {
            if (layer->setPosition(s.x, s.y))
                flags |= eTraversalNeeded;
        }
        if (what & layer_state_t::eLayerChanged) {
            // NOTE: index needs to be calculated before we update the state
            ssize_t idx = mCurrentState.layersSortedByZ.indexOf(layer);
            if (layer->setLayer(s.z)) {
                mCurrentState.layersSortedByZ.removeAt(idx);
                mCurrentState.layersSortedByZ.add(layer);
                // we need traversal (state changed)
                // AND transaction (list changed)
                flags |= eTransactionNeeded|eTraversalNeeded;
            }
        }
        if (what & layer_state_t::eSizeChanged) {
            if (layer->setSize(s.w, s.h)) {
                flags |= eTraversalNeeded;
            }
        }
        if (what & layer_state_t::eAlphaChanged) {
            if (layer->setAlpha(uint8_t(255.0f*s.alpha+0.5f)))
                flags |= eTraversalNeeded;
        }
        if (what & layer_state_t::eMatrixChanged) {
            if (layer->setMatrix(s.matrix))
                flags |= eTraversalNeeded;
        }
        if (what & layer_state_t::eTransparentRegionChanged) {
            if (layer->setTransparentRegionHint(s.transparentRegion))
                flags |= eTraversalNeeded;
        }
        if (what & layer_state_t::eVisibilityChanged) {
            if (layer->setFlags(s.flags, s.mask))
                flags |= eTraversalNeeded;
        }
        if (what & layer_state_t::eCropChanged) {
            if (layer->setCrop(s.crop))
                flags |= eTraversalNeeded;
        }
        if (what & layer_state_t::eLayerStackChanged) {
            // NOTE: index needs to be calculated before we update the state
            ssize_t idx = mCurrentState.layersSortedByZ.indexOf(layer);
            if (layer->setLayerStack(s.layerStack)) {
                mCurrentState.layersSortedByZ.removeAt(idx);
                mCurrentState.layersSortedByZ.add(layer);
                // we need traversal (state changed)
                // AND transaction (list changed)
                flags |= eTransactionNeeded|eTraversalNeeded;
            }
        }
    }
    return flags;
}

sp<ISurface> SurfaceFlinger::createLayer(
        ISurfaceComposerClient::surface_data_t* params,
        const String8& name,
        const sp<Client>& client,
        DisplayID d, uint32_t w, uint32_t h, PixelFormat format,
        uint32_t flags)
{
    sp<LayerBaseClient> layer;
    sp<ISurface> surfaceHandle;

    if (int32_t(w|h) < 0) {
        ALOGE("createLayer() failed, w or h is negative (w=%d, h=%d)",
                int(w), int(h));
        return surfaceHandle;
    }

    //ALOGD("createLayer for (%d x %d), name=%s", w, h, name.string());
    switch (flags & ISurfaceComposerClient::eFXSurfaceMask) {
        case ISurfaceComposerClient::eFXSurfaceNormal:
            layer = createNormalLayer(client, d, w, h, flags, format);
            break;
        case ISurfaceComposerClient::eFXSurfaceBlur:
        case ISurfaceComposerClient::eFXSurfaceDim:
            layer = createDimLayer(client, d, w, h, flags);
            break;
        case ISurfaceComposerClient::eFXSurfaceScreenshot:
            layer = createScreenshotLayer(client, d, w, h, flags);
            break;
    }

    if (layer != 0) {
        layer->initStates(w, h, flags);
        layer->setName(name);
        ssize_t token = addClientLayer(client, layer);
        surfaceHandle = layer->getSurface();
        if (surfaceHandle != 0) {
            params->token = token;
            params->identity = layer->getIdentity();
        }
        setTransactionFlags(eTransactionNeeded);
    }

    return surfaceHandle;
}

sp<Layer> SurfaceFlinger::createNormalLayer(
        const sp<Client>& client, DisplayID display,
        uint32_t w, uint32_t h, uint32_t flags,
        PixelFormat& format)
{
    // initialize the surfaces
    switch (format) {
    case PIXEL_FORMAT_TRANSPARENT:
    case PIXEL_FORMAT_TRANSLUCENT:
        format = PIXEL_FORMAT_RGBA_8888;
        break;
    case PIXEL_FORMAT_OPAQUE:
#ifdef NO_RGBX_8888
        format = PIXEL_FORMAT_RGB_565;
#else
        format = PIXEL_FORMAT_RGBX_8888;
#endif
        break;
    }

#ifdef NO_RGBX_8888
    if (format == PIXEL_FORMAT_RGBX_8888)
        format = PIXEL_FORMAT_RGBA_8888;
#endif

    sp<Layer> layer = new Layer(this, display, client);
    status_t err = layer->setBuffers(w, h, format, flags);
    if (CC_LIKELY(err != NO_ERROR)) {
        ALOGE("createNormalLayer() failed (%s)", strerror(-err));
        layer.clear();
    }
    return layer;
}

sp<LayerDim> SurfaceFlinger::createDimLayer(
        const sp<Client>& client, DisplayID display,
        uint32_t w, uint32_t h, uint32_t flags)
{
    sp<LayerDim> layer = new LayerDim(this, display, client);
    return layer;
}

sp<LayerScreenshot> SurfaceFlinger::createScreenshotLayer(
        const sp<Client>& client, DisplayID display,
        uint32_t w, uint32_t h, uint32_t flags)
{
    sp<LayerScreenshot> layer = new LayerScreenshot(this, display, client);
    return layer;
}

status_t SurfaceFlinger::onLayerRemoved(const sp<Client>& client, SurfaceID sid)
{
    /*
     * called by the window manager, when a surface should be marked for
     * destruction.
     *
     * The surface is removed from the current and drawing lists, but placed
     * in the purgatory queue, so it's not destroyed right-away (we need
     * to wait for all client's references to go away first).
     */

    status_t err = NAME_NOT_FOUND;
    Mutex::Autolock _l(mStateLock);
    sp<LayerBaseClient> layer = client->getLayerUser(sid);

    if (layer != 0) {
        err = purgatorizeLayer_l(layer);
        if (err == NO_ERROR) {
            setTransactionFlags(eTransactionNeeded);
        }
    }
    return err;
}

status_t SurfaceFlinger::onLayerDestroyed(const wp<LayerBaseClient>& layer)
{
    // called by ~ISurface() when all references are gone
    status_t err = NO_ERROR;
    sp<LayerBaseClient> l(layer.promote());
    if (l != NULL) {
        Mutex::Autolock _l(mStateLock);
        err = removeLayer_l(l);
        if (err == NAME_NOT_FOUND) {
            // The surface wasn't in the current list, which means it was
            // removed already, which means it is in the purgatory,
            // and need to be removed from there.
            ssize_t idx = mLayerPurgatory.remove(l);
            ALOGE_IF(idx < 0,
                    "layer=%p is not in the purgatory list", l.get());
        }
        ALOGE_IF(err<0 && err != NAME_NOT_FOUND,
                "error removing layer=%p (%s)", l.get(), strerror(-err));
    }
    return err;
}

// ---------------------------------------------------------------------------

void SurfaceFlinger::onScreenAcquired() {
    ALOGD("Screen about to return, flinger = %p", this);
    sp<const DisplayDevice> hw(getDefaultDisplayDevice()); // XXX: this should be per DisplayDevice
    getHwComposer().acquire();
    hw->acquireScreen();
    mEventThread->onScreenAcquired();
}

void SurfaceFlinger::onScreenReleased() {
    ALOGD("About to give-up screen, flinger = %p", this);
    sp<const DisplayDevice> hw(getDefaultDisplayDevice()); // XXX: this should be per DisplayDevice
    if (hw->isScreenAcquired()) {
        mEventThread->onScreenReleased();
        hw->releaseScreen();
        getHwComposer().release();
        // from this point on, SF will stop drawing
    }
}

void SurfaceFlinger::unblank() {
    class MessageScreenAcquired : public MessageBase {
        SurfaceFlinger* flinger;
    public:
        MessageScreenAcquired(SurfaceFlinger* flinger) : flinger(flinger) { }
        virtual bool handler() {
            flinger->onScreenAcquired();
            return true;
        }
    };
    sp<MessageBase> msg = new MessageScreenAcquired(this);
    postMessageSync(msg);
}

void SurfaceFlinger::blank() {
    class MessageScreenReleased : public MessageBase {
        SurfaceFlinger* flinger;
    public:
        MessageScreenReleased(SurfaceFlinger* flinger) : flinger(flinger) { }
        virtual bool handler() {
            flinger->onScreenReleased();
            return true;
        }
    };
    sp<MessageBase> msg = new MessageScreenReleased(this);
    postMessageSync(msg);
}

// ---------------------------------------------------------------------------

status_t SurfaceFlinger::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 4096;
    char buffer[SIZE];
    String8 result;

    if (!PermissionCache::checkCallingPermission(sDump)) {
        snprintf(buffer, SIZE, "Permission Denial: "
                "can't dump SurfaceFlinger from pid=%d, uid=%d\n",
                IPCThreadState::self()->getCallingPid(),
                IPCThreadState::self()->getCallingUid());
        result.append(buffer);
    } else {
        // Try to get the main lock, but don't insist if we can't
        // (this would indicate SF is stuck, but we want to be able to
        // print something in dumpsys).
        int retry = 3;
        while (mStateLock.tryLock()<0 && --retry>=0) {
            usleep(1000000);
        }
        const bool locked(retry >= 0);
        if (!locked) {
            snprintf(buffer, SIZE,
                    "SurfaceFlinger appears to be unresponsive, "
                    "dumping anyways (no locks held)\n");
            result.append(buffer);
        }

        bool dumpAll = true;
        size_t index = 0;
        size_t numArgs = args.size();
        if (numArgs) {
            if ((index < numArgs) &&
                    (args[index] == String16("--list"))) {
                index++;
                listLayersLocked(args, index, result, buffer, SIZE);
                dumpAll = false;
            }

            if ((index < numArgs) &&
                    (args[index] == String16("--latency"))) {
                index++;
                dumpStatsLocked(args, index, result, buffer, SIZE);
                dumpAll = false;
            }

            if ((index < numArgs) &&
                    (args[index] == String16("--latency-clear"))) {
                index++;
                clearStatsLocked(args, index, result, buffer, SIZE);
                dumpAll = false;
            }
        }

        if (dumpAll) {
            dumpAllLocked(result, buffer, SIZE);
        }

        if (locked) {
            mStateLock.unlock();
        }
    }
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

void SurfaceFlinger::listLayersLocked(const Vector<String16>& args, size_t& index,
        String8& result, char* buffer, size_t SIZE) const
{
    const LayerVector& currentLayers = mCurrentState.layersSortedByZ;
    const size_t count = currentLayers.size();
    for (size_t i=0 ; i<count ; i++) {
        const sp<LayerBase>& layer(currentLayers[i]);
        snprintf(buffer, SIZE, "%s\n", layer->getName().string());
        result.append(buffer);
    }
}

void SurfaceFlinger::dumpStatsLocked(const Vector<String16>& args, size_t& index,
        String8& result, char* buffer, size_t SIZE) const
{
    String8 name;
    if (index < args.size()) {
        name = String8(args[index]);
        index++;
    }

    const LayerVector& currentLayers = mCurrentState.layersSortedByZ;
    const size_t count = currentLayers.size();
    for (size_t i=0 ; i<count ; i++) {
        const sp<LayerBase>& layer(currentLayers[i]);
        if (name.isEmpty()) {
            snprintf(buffer, SIZE, "%s\n", layer->getName().string());
            result.append(buffer);
        }
        if (name.isEmpty() || (name == layer->getName())) {
            layer->dumpStats(result, buffer, SIZE);
        }
    }
}

void SurfaceFlinger::clearStatsLocked(const Vector<String16>& args, size_t& index,
        String8& result, char* buffer, size_t SIZE) const
{
    String8 name;
    if (index < args.size()) {
        name = String8(args[index]);
        index++;
    }

    const LayerVector& currentLayers = mCurrentState.layersSortedByZ;
    const size_t count = currentLayers.size();
    for (size_t i=0 ; i<count ; i++) {
        const sp<LayerBase>& layer(currentLayers[i]);
        if (name.isEmpty() || (name == layer->getName())) {
            layer->clearStats();
        }
    }
}

void SurfaceFlinger::dumpAllLocked(
        String8& result, char* buffer, size_t SIZE) const
{
    // figure out if we're stuck somewhere
    const nsecs_t now = systemTime();
    const nsecs_t inSwapBuffers(mDebugInSwapBuffers);
    const nsecs_t inTransaction(mDebugInTransaction);
    nsecs_t inSwapBuffersDuration = (inSwapBuffers) ? now-inSwapBuffers : 0;
    nsecs_t inTransactionDuration = (inTransaction) ? now-inTransaction : 0;

    /*
     * Dump the visible layer list
     */
    const LayerVector& currentLayers = mCurrentState.layersSortedByZ;
    const size_t count = currentLayers.size();
    snprintf(buffer, SIZE, "Visible layers (count = %d)\n", count);
    result.append(buffer);
    for (size_t i=0 ; i<count ; i++) {
        const sp<LayerBase>& layer(currentLayers[i]);
        layer->dump(result, buffer, SIZE);
    }

    /*
     * Dump the layers in the purgatory
     */

    const size_t purgatorySize = mLayerPurgatory.size();
    snprintf(buffer, SIZE, "Purgatory state (%d entries)\n", purgatorySize);
    result.append(buffer);
    for (size_t i=0 ; i<purgatorySize ; i++) {
        const sp<LayerBase>& layer(mLayerPurgatory.itemAt(i));
        layer->shortDump(result, buffer, SIZE);
    }

    /*
     * Dump SurfaceFlinger global state
     */

    snprintf(buffer, SIZE, "SurfaceFlinger global state:\n");
    result.append(buffer);

    HWComposer& hwc(getHwComposer());
    sp<const DisplayDevice> hw(getDefaultDisplayDevice());
    const GLExtensions& extensions(GLExtensions::getInstance());
    snprintf(buffer, SIZE, "GLES: %s, %s, %s\n",
            extensions.getVendor(),
            extensions.getRenderer(),
            extensions.getVersion());
    result.append(buffer);

    snprintf(buffer, SIZE, "EGL : %s\n",
            eglQueryString(mEGLDisplay, EGL_VERSION_HW_ANDROID));
    result.append(buffer);

    snprintf(buffer, SIZE, "EXTS: %s\n", extensions.getExtension());
    result.append(buffer);

    hw->undefinedRegion.dump(result, "undefinedRegion");
    snprintf(buffer, SIZE,
            "  orientation=%d, canDraw=%d\n",
            hw->getOrientation(), hw->canDraw());
    result.append(buffer);
    snprintf(buffer, SIZE,
            "  last eglSwapBuffers() time: %f us\n"
            "  last transaction time     : %f us\n"
            "  transaction-flags         : %08x\n"
            "  refresh-rate              : %f fps\n"
            "  x-dpi                     : %f\n"
            "  y-dpi                     : %f\n",
            mLastSwapBufferTime/1000.0,
            mLastTransactionTime/1000.0,
            mTransactionFlags,
            1e9 / hwc.getRefreshPeriod(),
            hwc.getDpiX(),
            hwc.getDpiY());
    result.append(buffer);

    snprintf(buffer, SIZE, "  eglSwapBuffers time: %f us\n",
            inSwapBuffersDuration/1000.0);
    result.append(buffer);

    snprintf(buffer, SIZE, "  transaction time: %f us\n",
            inTransactionDuration/1000.0);
    result.append(buffer);

    /*
     * VSYNC state
     */
    mEventThread->dump(result, buffer, SIZE);

    /*
     * Dump HWComposer state
     */
    snprintf(buffer, SIZE, "h/w composer state:\n");
    result.append(buffer);
    snprintf(buffer, SIZE, "  h/w composer %s and %s\n",
            hwc.initCheck()==NO_ERROR ? "present" : "not present",
                    (mDebugDisableHWC || mDebugRegion) ? "disabled" : "enabled");
    result.append(buffer);
    hwc.dump(result, buffer, SIZE, hw->getVisibleLayersSortedByZ());

    /*
     * Dump gralloc state
     */
    const GraphicBufferAllocator& alloc(GraphicBufferAllocator::get());
    alloc.dump(result);
    hw->dump(result);
}

status_t SurfaceFlinger::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {
        case CREATE_CONNECTION:
        case SET_TRANSACTION_STATE:
        case BOOT_FINISHED:
        case BLANK:
        case UNBLANK:
        {
            // codes that require permission check
            IPCThreadState* ipc = IPCThreadState::self();
            const int pid = ipc->getCallingPid();
            const int uid = ipc->getCallingUid();
            if ((uid != AID_GRAPHICS) &&
                    !PermissionCache::checkPermission(sAccessSurfaceFlinger, pid, uid)) {
                ALOGE("Permission Denial: "
                        "can't access SurfaceFlinger pid=%d, uid=%d", pid, uid);
                return PERMISSION_DENIED;
            }
            break;
        }
        case CAPTURE_SCREEN:
        {
            // codes that require permission check
            IPCThreadState* ipc = IPCThreadState::self();
            const int pid = ipc->getCallingPid();
            const int uid = ipc->getCallingUid();
            if ((uid != AID_GRAPHICS) &&
                    !PermissionCache::checkPermission(sReadFramebuffer, pid, uid)) {
                ALOGE("Permission Denial: "
                        "can't read framebuffer pid=%d, uid=%d", pid, uid);
                return PERMISSION_DENIED;
            }
            break;
        }
    }

    status_t err = BnSurfaceComposer::onTransact(code, data, reply, flags);
    if (err == UNKNOWN_TRANSACTION || err == PERMISSION_DENIED) {
        CHECK_INTERFACE(ISurfaceComposer, data, reply);
        if (CC_UNLIKELY(!PermissionCache::checkCallingPermission(sHardwareTest))) {
            IPCThreadState* ipc = IPCThreadState::self();
            const int pid = ipc->getCallingPid();
            const int uid = ipc->getCallingUid();
            ALOGE("Permission Denial: "
                    "can't access SurfaceFlinger pid=%d, uid=%d", pid, uid);
            return PERMISSION_DENIED;
        }
        int n;
        switch (code) {
            case 1000: // SHOW_CPU, NOT SUPPORTED ANYMORE
            case 1001: // SHOW_FPS, NOT SUPPORTED ANYMORE
                return NO_ERROR;
            case 1002:  // SHOW_UPDATES
                n = data.readInt32();
                mDebugRegion = n ? n : (mDebugRegion ? 0 : 1);
                invalidateHwcGeometry();
                repaintEverything();
                return NO_ERROR;
            case 1004:{ // repaint everything
                repaintEverything();
                return NO_ERROR;
            }
            case 1005:{ // force transaction
                setTransactionFlags(
                        eTransactionNeeded|
                        eDisplayTransactionNeeded|
                        eTraversalNeeded);
                return NO_ERROR;
            }
            case 1006:{ // send empty update
                signalRefresh();
                return NO_ERROR;
            }
            case 1008:  // toggle use of hw composer
                n = data.readInt32();
                mDebugDisableHWC = n ? 1 : 0;
                invalidateHwcGeometry();
                repaintEverything();
                return NO_ERROR;
            case 1009:  // toggle use of transform hint
                n = data.readInt32();
                mDebugDisableTransformHint = n ? 1 : 0;
                invalidateHwcGeometry();
                repaintEverything();
                return NO_ERROR;
            case 1010:  // interrogate.
                reply->writeInt32(0);
                reply->writeInt32(0);
                reply->writeInt32(mDebugRegion);
                reply->writeInt32(0);
                reply->writeInt32(mDebugDisableHWC);
                return NO_ERROR;
            case 1013: {
                Mutex::Autolock _l(mStateLock);
                sp<const DisplayDevice> hw(getDefaultDisplayDevice());
                reply->writeInt32(hw->getPageFlipCount());
            }
            return NO_ERROR;
        }
    }
    return err;
}

void SurfaceFlinger::repaintEverything() {
    android_atomic_or(1, &mRepaintEverything);
    signalTransaction();
}

// ---------------------------------------------------------------------------

status_t SurfaceFlinger::renderScreenToTexture(DisplayID dpy,
        GLuint* textureName, GLfloat* uOut, GLfloat* vOut)
{
    Mutex::Autolock _l(mStateLock);
    return renderScreenToTextureLocked(dpy, textureName, uOut, vOut);
}

status_t SurfaceFlinger::renderScreenToTextureLocked(DisplayID dpy,
        GLuint* textureName, GLfloat* uOut, GLfloat* vOut)
{
    ATRACE_CALL();

    if (!GLExtensions::getInstance().haveFramebufferObject())
        return INVALID_OPERATION;

    // get screen geometry
    sp<const DisplayDevice> hw(getDisplayDevice(dpy));
    const uint32_t hw_w = hw->getWidth();
    const uint32_t hw_h = hw->getHeight();
    GLfloat u = 1;
    GLfloat v = 1;

    // make sure to clear all GL error flags
    while ( glGetError() != GL_NO_ERROR ) ;

    // create a FBO
    GLuint name, tname;
    glGenTextures(1, &tname);
    glBindTexture(GL_TEXTURE_2D, tname);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
            hw_w, hw_h, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
    if (glGetError() != GL_NO_ERROR) {
        while ( glGetError() != GL_NO_ERROR ) ;
        GLint tw = (2 << (31 - clz(hw_w)));
        GLint th = (2 << (31 - clz(hw_h)));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                tw, th, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
        u = GLfloat(hw_w) / tw;
        v = GLfloat(hw_h) / th;
    }
    glGenFramebuffersOES(1, &name);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, name);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES,
            GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, tname, 0);

    // redraw the screen entirely...
    glDisable(GL_TEXTURE_EXTERNAL_OES);
    glDisable(GL_TEXTURE_2D);
    glClearColor(0,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    const Vector< sp<LayerBase> >& layers(hw->getVisibleLayersSortedByZ());
    const size_t count = layers.size();
    for (size_t i=0 ; i<count ; ++i) {
        const sp<LayerBase>& layer(layers[i]);
        layer->draw(hw);
    }

    hw->compositionComplete();

    // back to main framebuffer
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
    glDeleteFramebuffersOES(1, &name);

    *textureName = tname;
    *uOut = u;
    *vOut = v;
    return NO_ERROR;
}

// ---------------------------------------------------------------------------

status_t SurfaceFlinger::captureScreenImplLocked(DisplayID dpy,
        sp<IMemoryHeap>* heap,
        uint32_t* w, uint32_t* h, PixelFormat* f,
        uint32_t sw, uint32_t sh,
        uint32_t minLayerZ, uint32_t maxLayerZ)
{
    ATRACE_CALL();

    status_t result = PERMISSION_DENIED;

    // only one display supported for now
    if (CC_UNLIKELY(uint32_t(dpy) >= DISPLAY_COUNT)) {
        ALOGE("invalid display %d", dpy);
        return BAD_VALUE;
    }

    if (!GLExtensions::getInstance().haveFramebufferObject()) {
        return INVALID_OPERATION;
    }

    // get screen geometry
    sp<const DisplayDevice> hw(getDisplayDevice(dpy));
    const uint32_t hw_w = hw->getWidth();
    const uint32_t hw_h = hw->getHeight();

    // if we have secure windows on this display, never allow the screen capture
    if (hw->getSecureLayerVisible()) {
        ALOGW("FB is protected: PERMISSION_DENIED");
        return PERMISSION_DENIED;
    }

    if ((sw > hw_w) || (sh > hw_h)) {
        ALOGE("size mismatch (%d, %d) > (%d, %d)", sw, sh, hw_w, hw_h);
        return BAD_VALUE;
    }

    sw = (!sw) ? hw_w : sw;
    sh = (!sh) ? hw_h : sh;
    const size_t size = sw * sh * 4;
    const bool filtering = sw != hw_w || sh != hw_h;

//    ALOGD("screenshot: sw=%d, sh=%d, minZ=%d, maxZ=%d",
//            sw, sh, minLayerZ, maxLayerZ);

    // make sure to clear all GL error flags
    while ( glGetError() != GL_NO_ERROR ) ;

    // create a FBO
    GLuint name, tname;
    glGenRenderbuffersOES(1, &tname);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, tname);
    glRenderbufferStorageOES(GL_RENDERBUFFER_OES, GL_RGBA8_OES, sw, sh);

    glGenFramebuffersOES(1, &name);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, name);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES,
            GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, tname);

    GLenum status = glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES);

    if (status == GL_FRAMEBUFFER_COMPLETE_OES) {

        // invert everything, b/c glReadPixel() below will invert the FB
        glViewport(0, 0, sw, sh);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrthof(0, hw_w, hw_h, 0, 0, 1);
        glMatrixMode(GL_MODELVIEW);

        // redraw the screen entirely...
        glClearColor(0,0,0,1);
        glClear(GL_COLOR_BUFFER_BIT);

        const LayerVector& layers(mDrawingState.layersSortedByZ);
        const size_t count = layers.size();
        for (size_t i=0 ; i<count ; ++i) {
            const sp<LayerBase>& layer(layers[i]);
            const uint32_t flags = layer->drawingState().flags;
            if (!(flags & layer_state_t::eLayerHidden)) {
                const uint32_t z = layer->drawingState().z;
                if (z >= minLayerZ && z <= maxLayerZ) {
                    if (filtering) layer->setFiltering(true);
                    layer->draw(hw);
                    if (filtering) layer->setFiltering(false);
                }
            }
        }

        // check for errors and return screen capture
        if (glGetError() != GL_NO_ERROR) {
            // error while rendering
            result = INVALID_OPERATION;
        } else {
            // allocate shared memory large enough to hold the
            // screen capture
            sp<MemoryHeapBase> base(
                    new MemoryHeapBase(size, 0, "screen-capture") );
            void* const ptr = base->getBase();
            if (ptr) {
                // capture the screen with glReadPixels()
                ScopedTrace _t(ATRACE_TAG, "glReadPixels");
                glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, ptr);
                if (glGetError() == GL_NO_ERROR) {
                    *heap = base;
                    *w = sw;
                    *h = sh;
                    *f = PIXEL_FORMAT_RGBA_8888;
                    result = NO_ERROR;
                }
            } else {
                result = NO_MEMORY;
            }
        }
        glViewport(0, 0, hw_w, hw_h);
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
    } else {
        result = BAD_VALUE;
    }

    // release FBO resources
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
    glDeleteRenderbuffersOES(1, &tname);
    glDeleteFramebuffersOES(1, &name);

    hw->compositionComplete();

//    ALOGD("screenshot: result = %s", result<0 ? strerror(result) : "OK");

    return result;
}


status_t SurfaceFlinger::captureScreen(DisplayID dpy,
        sp<IMemoryHeap>* heap,
        uint32_t* width, uint32_t* height, PixelFormat* format,
        uint32_t sw, uint32_t sh,
        uint32_t minLayerZ, uint32_t maxLayerZ)
{
    // only one display supported for now
    if (CC_UNLIKELY(uint32_t(dpy) >= DISPLAY_COUNT))
        return BAD_VALUE;

    if (!GLExtensions::getInstance().haveFramebufferObject())
        return INVALID_OPERATION;

    class MessageCaptureScreen : public MessageBase {
        SurfaceFlinger* flinger;
        DisplayID dpy;
        sp<IMemoryHeap>* heap;
        uint32_t* w;
        uint32_t* h;
        PixelFormat* f;
        uint32_t sw;
        uint32_t sh;
        uint32_t minLayerZ;
        uint32_t maxLayerZ;
        status_t result;
    public:
        MessageCaptureScreen(SurfaceFlinger* flinger, DisplayID dpy,
                sp<IMemoryHeap>* heap, uint32_t* w, uint32_t* h, PixelFormat* f,
                uint32_t sw, uint32_t sh,
                uint32_t minLayerZ, uint32_t maxLayerZ)
            : flinger(flinger), dpy(dpy),
              heap(heap), w(w), h(h), f(f), sw(sw), sh(sh),
              minLayerZ(minLayerZ), maxLayerZ(maxLayerZ),
              result(PERMISSION_DENIED)
        {
        }
        status_t getResult() const {
            return result;
        }
        virtual bool handler() {
            Mutex::Autolock _l(flinger->mStateLock);
            result = flinger->captureScreenImplLocked(dpy,
                    heap, w, h, f, sw, sh, minLayerZ, maxLayerZ);
            return true;
        }
    };

    sp<MessageBase> msg = new MessageCaptureScreen(this,
            dpy, heap, width, height, format, sw, sh, minLayerZ, maxLayerZ);
    status_t res = postMessageSync(msg);
    if (res == NO_ERROR) {
        res = static_cast<MessageCaptureScreen*>( msg.get() )->getResult();
    }
    return res;
}

// ---------------------------------------------------------------------------

SurfaceFlinger::LayerVector::LayerVector() {
}

SurfaceFlinger::LayerVector::LayerVector(const LayerVector& rhs)
    : SortedVector<sp<LayerBase> >(rhs) {
}

int SurfaceFlinger::LayerVector::do_compare(const void* lhs,
    const void* rhs) const
{
    // sort layers per layer-stack, then by z-order and finally by sequence
    const sp<LayerBase>& l(*reinterpret_cast<const sp<LayerBase>*>(lhs));
    const sp<LayerBase>& r(*reinterpret_cast<const sp<LayerBase>*>(rhs));

    uint32_t ls = l->currentState().layerStack;
    uint32_t rs = r->currentState().layerStack;
    if (ls != rs)
        return ls - rs;

    uint32_t lz = l->currentState().z;
    uint32_t rz = r->currentState().z;
    if (lz != rz)
        return lz - rz;

    return l->sequence - r->sequence;
}

// ---------------------------------------------------------------------------

SurfaceFlinger::DisplayDeviceState::DisplayDeviceState() : id(-1) {
}

SurfaceFlinger::DisplayDeviceState::DisplayDeviceState(int32_t id)
    : id(id), layerStack(0), orientation(0) {
}

// ---------------------------------------------------------------------------

GraphicBufferAlloc::GraphicBufferAlloc() {}

GraphicBufferAlloc::~GraphicBufferAlloc() {}

sp<GraphicBuffer> GraphicBufferAlloc::createGraphicBuffer(uint32_t w, uint32_t h,
        PixelFormat format, uint32_t usage, status_t* error) {
    sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(w, h, format, usage));
    status_t err = graphicBuffer->initCheck();
    *error = err;
    if (err != 0 || graphicBuffer->handle == 0) {
        if (err == NO_MEMORY) {
            GraphicBuffer::dumpAllocationsToSystemLog();
        }
        ALOGE("GraphicBufferAlloc::createGraphicBuffer(w=%d, h=%d) "
             "failed (%s), handle=%p",
                w, h, strerror(-err), graphicBuffer->handle);
        return 0;
    }
    return graphicBuffer;
}

// ---------------------------------------------------------------------------

}; // namespace android
