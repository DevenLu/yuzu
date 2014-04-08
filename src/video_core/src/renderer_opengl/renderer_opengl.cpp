/**
 * Copyright (C) 2014 Citra Emulator
 *
 * @file    renderer_opengl.cpp
 * @author  bunnei
 * @date    2014-04-05
 * @brief   Renderer for OpenGL 3.x
 *
 * @section LICENSE
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details at
 * http://www.gnu.org/copyleft/gpl.html
 *
 * Official project repository can be found at:
 * http://code.google.com/p/gekko-gc-emu/
 */

#include "mem_map.h"
#include "video_core.h"
#include "renderer_opengl/renderer_opengl.h"


/// RendererOpenGL constructor
RendererOpenGL::RendererOpenGL() {
    memset(m_fbo, 0, sizeof(m_fbo));  
    memset(m_fbo_rbo, 0, sizeof(m_fbo_rbo));  
    memset(m_fbo_depth_buffers, 0, sizeof(m_fbo_depth_buffers));

    m_resolution_width = max(VideoCore::kScreenTopWidth, VideoCore::kScreenBottomWidth);
    m_resolution_height = VideoCore::kScreenTopHeight + VideoCore::kScreenBottomHeight;

    m_xfb_texture_top = 0;
    m_xfb_texture_bottom = 0;

    m_xfb_top = 0;
    m_xfb_bottom = 0;
}

/// RendererOpenGL destructor
RendererOpenGL::~RendererOpenGL() {
}

/// Swap buffers (render frame)
void RendererOpenGL::SwapBuffers() {
    // EFB->XFB copy
    // TODO(bunnei): This is a hack and does not belong here. The copy should be triggered by some 
    // register write We're also treating both framebuffers as a single one in OpenGL.
    Rect framebuffer_size(0, 0, m_resolution_width, m_resolution_height);
    RenderXFB(framebuffer_size, framebuffer_size);

    // XFB->Window copy
    RenderFramebuffer();

    // Swap buffers
    m_render_window->PollEvents();
    m_render_window->SwapBuffers();

    // Switch back to EFB and clear
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo[kFramebuffer_EFB]);
}

/**
 * Helper function to flip framebuffer from left-to-right to top-to-bottom
 * @param addr Address of framebuffer in RAM
 * @param out Pointer to output buffer with flipped framebuffer
 * @todo Early on hack... I'd like to find a more efficient way of doing this /bunnei
 */
void RendererOpenGL::FlipFramebuffer(u32 addr, u8* out) {
    u8* in = Memory::GetPointer(addr);
    for (int y = 0; y < VideoCore::kScreenTopHeight; y++) {
        for (int x = 0; x < VideoCore::kScreenTopWidth; x++) {
            int in_coord = (VideoCore::kScreenTopHeight * 3 * x) + (VideoCore::kScreenTopHeight * 3)
                - (3 * y + 3);
            int out_coord = (VideoCore::kScreenTopWidth * y * 3) + (x * 3);

            out[out_coord + 0] = in[in_coord + 0];
            out[out_coord + 1] = in[in_coord + 1];
            out[out_coord + 2] = in[in_coord + 2];
        }
    }
}

/** 
 * Renders external framebuffer (XFB)
 * @param src_rect Source rectangle in XFB to copy
 * @param dst_rect Destination rectangle in output framebuffer to copy to
 */
void RendererOpenGL::RenderXFB(const Rect& src_rect, const Rect& dst_rect) {  

    FlipFramebuffer(0x20282160, m_xfb_top_flipped);
    FlipFramebuffer(0x202118E0, m_xfb_bottom_flipped);

    // Blit the top framebuffer
    // ------------------------

    // Update textures with contents of XFB in RAM - top
    glBindTexture(GL_TEXTURE_2D, m_xfb_texture_top);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VideoCore::kScreenTopWidth, VideoCore::kScreenTopHeight,
        GL_RGB, GL_UNSIGNED_BYTE, m_xfb_top_flipped);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Render target is destination framebuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo[kFramebuffer_VirtualXFB]);
    glViewport(0, 0, VideoCore::kScreenTopWidth, VideoCore::kScreenTopHeight);

    // Render source is our EFB
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_xfb_top);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    // Blit
    glBlitFramebuffer(src_rect.x0_, src_rect.y0_, src_rect.x1_, src_rect.y1_, 
                      dst_rect.x0_, dst_rect.y1_, dst_rect.x1_, dst_rect.y0_,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // Blit the bottom framebuffer
    // ---------------------------

    // Update textures with contents of XFB in RAM - bottom
    glBindTexture(GL_TEXTURE_2D, m_xfb_texture_bottom);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VideoCore::kScreenTopWidth, VideoCore::kScreenTopHeight,
        GL_RGB, GL_UNSIGNED_BYTE, m_xfb_bottom_flipped);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Render target is destination framebuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo[kFramebuffer_VirtualXFB]);
    glViewport(0, 0,
        VideoCore::kScreenBottomWidth, VideoCore::kScreenBottomHeight);

    // Render source is our EFB
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_xfb_bottom);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    // Blit
    int offset = (VideoCore::kScreenTopWidth - VideoCore::kScreenBottomWidth) / 2;
    glBlitFramebuffer(0,0, VideoCore::kScreenBottomWidth, VideoCore::kScreenBottomHeight, 
                      offset, VideoCore::kScreenBottomHeight, VideoCore::kScreenBottomWidth + offset, 0,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

/// Initialize the FBO
void RendererOpenGL::InitFramebuffer() {
    // TODO(bunnei): This should probably be implemented with the top screen and bottom screen as 
    // separate framebuffers

    // Init the FBOs
    // -------------

    glGenFramebuffers(kMaxFramebuffers, m_fbo); // Generate primary framebuffer
    glGenRenderbuffers(kMaxFramebuffers, m_fbo_rbo); // Generate primary RBOs
    glGenRenderbuffers(kMaxFramebuffers, m_fbo_depth_buffers); // Generate primary depth buffer

    for (int i = 0; i < kMaxFramebuffers; i++) {
        // Generate color buffer storage
        glBindRenderbuffer(GL_RENDERBUFFER, m_fbo_rbo[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, VideoCore::kScreenTopWidth, 
            VideoCore::kScreenTopHeight + VideoCore::kScreenBottomHeight);

        // Generate depth buffer storage
        glBindRenderbuffer(GL_RENDERBUFFER, m_fbo_depth_buffers[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32, VideoCore::kScreenTopWidth, 
            VideoCore::kScreenTopHeight + VideoCore::kScreenBottomHeight);

        // Attach the buffers
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo[i]);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            GL_RENDERBUFFER, m_fbo_depth_buffers[i]);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_RENDERBUFFER, m_fbo_rbo[i]);

        // Check for completeness
        if (GL_FRAMEBUFFER_COMPLETE == glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)) {
            NOTICE_LOG(RENDER, "framebuffer(%d) initialized ok", i);
        } else {
            ERROR_LOG(RENDER, "couldn't create OpenGL frame buffer");
            exit(1);
        } 
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind our frame buffer(s)

    // Initialize framebuffer textures
    // -------------------------------

    // Create XFB textures
    glGenTextures(1, &m_xfb_texture_top);  
    glGenTextures(1, &m_xfb_texture_bottom);  

    // Alocate video memorry for XFB textures
    glBindTexture(GL_TEXTURE_2D, m_xfb_texture_top);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, VideoCore::kScreenTopWidth, VideoCore::kScreenTopHeight,
        0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindTexture(GL_TEXTURE_2D, m_xfb_texture_bottom);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, VideoCore::kScreenTopWidth, VideoCore::kScreenTopHeight,
        0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create the FBO and attach color/depth textures
    glGenFramebuffers(1, &m_xfb_top); // Generate framebuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xfb_top);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 
        m_xfb_texture_top, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glGenFramebuffers(1, &m_xfb_bottom); // Generate framebuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xfb_bottom);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 
        m_xfb_texture_bottom, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/// Blit the FBO to the OpenGL default framebuffer
void RendererOpenGL::RenderFramebuffer() {
    // Render target is default framebuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, m_resolution_width, m_resolution_height);

    // Render source is our XFB
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo[kFramebuffer_VirtualXFB]);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    // Blit
    glBlitFramebuffer(0, 0, m_resolution_width, m_resolution_height, 0, 0, m_resolution_width, 
        m_resolution_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Update the FPS count
    UpdateFramerate();

    // Rebind EFB
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo[kFramebuffer_EFB]);

    m_current_frame++;
}

/// Updates the framerate
void RendererOpenGL::UpdateFramerate() {
}

/** 
 * Set the emulator window to use for renderer
 * @param window EmuWindow handle to emulator window to use for rendering
 */
void RendererOpenGL::SetWindow(EmuWindow* window) {
    m_render_window = window;
}

/// Initialize the renderer
void RendererOpenGL::Init() {
    m_render_window->MakeCurrent();
    glShadeModel(GL_SMOOTH);


    glStencilFunc(GL_ALWAYS, 0, 0);
    glBlendFunc(GL_ONE, GL_ONE);

    glViewport(0, 0, m_resolution_width, m_resolution_height);

    glClearDepth(1.0f);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDepthFunc(GL_LEQUAL);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);

    glScissor(0, 0, m_resolution_width, m_resolution_height);
    glClearDepth(1.0f);

    GLenum err = glewInit();
    if (GLEW_OK != err) {
        ERROR_LOG(RENDER, "Failed to initialize GLEW! Error message: \"%s\". Exiting...", 
            glewGetErrorString(err));
        exit(-1);
    }

    // Initialize everything else
    // --------------------------

    InitFramebuffer();

    NOTICE_LOG(RENDER, "GL_VERSION: %s\n", glGetString(GL_VERSION));
}

/// Shutdown the renderer
void RendererOpenGL::ShutDown() {
}