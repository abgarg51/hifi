//
//  Created by Bradley Austin Davis on 2016/04/03
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "GLBackendTextureTransfer.h"

#include "GLBackendShared.h"

#ifdef THREADED_TEXTURE_TRANSFER
#include <gl/OffscreenGLCanvas.h>
#include <gl/QOpenGLContextWrapper.h>
#endif

using namespace gpu;

GLTextureTransferHelper::GLTextureTransferHelper() {
#ifdef THREADED_TEXTURE_TRANSFER
    _canvas = QSharedPointer<OffscreenGLCanvas>(new OffscreenGLCanvas(), &QObject::deleteLater);
    _canvas->create(QOpenGLContextWrapper::currentContext());
    if (!_canvas->makeCurrent()) {
        qFatal("Unable to create texture transfer context");
    }
    _canvas->doneCurrent();
    initialize(true, QThread::LowPriority);
    _canvas->moveToThreadWithContext(_thread);

    // Clean shutdown on UNIX, otherwise _canvas is freed early
    connect(qApp, &QCoreApplication::aboutToQuit, [&] { terminate(); });
#endif
}

GLTextureTransferHelper::~GLTextureTransferHelper() {
#ifdef THREADED_TEXTURE_TRANSFER
    if (isStillRunning()) {
        terminate();
    }
#endif
}

void GLTextureTransferHelper::transferTexture(const gpu::TexturePointer& texturePointer) {
    GLBackend::GLTexture* object = Backend::getGPUObject<GLBackend::GLTexture>(*texturePointer);
    Backend::incrementTextureGPUTransferCount();
#ifdef THREADED_TEXTURE_TRANSFER
    GLsync fence { 0 };
    //fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    //glFlush();

    TextureTransferPackage package { texturePointer, fence };
    object->setSyncState(GLBackend::GLTexture::Pending);
    queueItem(package);
#else
    object->withPreservedTexture([&] {
        do_transfer(*object);
    });
    object->_contentStamp = texturePointer->getDataStamp();
    object->setSyncState(GLBackend::GLTexture::Transferred);
#endif
}

void GLTextureTransferHelper::setup() {
#ifdef THREADED_TEXTURE_TRANSFER
    _canvas->makeCurrent();
#endif
}

void GLTextureTransferHelper::shutdown() {
#ifdef THREADED_TEXTURE_TRANSFER
    _canvas->doneCurrent();
    _canvas->moveToThreadWithContext(qApp->thread());
    _canvas.reset();
#endif
}

void GLTextureTransferHelper::do_transfer(GLBackend::GLTexture& texture) {
    texture.createTexture();
    texture.transfer();
    texture.updateSize();
    Backend::decrementTextureGPUTransferCount();
}

bool GLTextureTransferHelper::processQueueItems(const Queue& messages) {
    for (auto package : messages) {
        TexturePointer texturePointer = package.texture.lock();
        // Texture no longer exists, move on to the next
        if (!texturePointer) {
            continue;
        }

        if (package.fence) {
            glClientWaitSync(package.fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(package.fence);
            package.fence = 0;
        }

        GLBackend::GLTexture* object = Backend::getGPUObject<GLBackend::GLTexture>(*texturePointer);
        do_transfer(*object);
        glBindTexture(object->_target, 0);

        auto writeSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glClientWaitSync(writeSync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(writeSync);

        object->_contentStamp = texturePointer->getDataStamp();
        object->setSyncState(GLBackend::GLTexture::Transferred);
    }
    return true;
}
