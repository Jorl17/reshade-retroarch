// OpenGL backend of the test host (see backend.h): a legacy WGL context on the window.
// Pictures are uploaded once as textures and copied into the window's back buffer with
// glBlitFramebuffer.
#include "backend.h"

#include <GL/gl.h>

#include <cstdio>
#include <map>

namespace
{
// OpenGL 3.0 framebuffer-object functions and constants. The gl.h that comes with Windows
// only declares OpenGL 1.1, so these are declared here and loaded at run time with
// wglGetProcAddress.
constexpr GLenum kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9, kColorAttachment0 = 0x8CE0,
                 kFramebufferComplete = 0x8CD5, kRGBA8 = 0x8058, kPixelPackBuffer = 0x88EB,
                 kPixelUnpackBuffer = 0x88EC;
using PFNBindBuffer = void(APIENTRY *)(GLenum, GLuint);
using PFNGenFramebuffers = void(APIENTRY *)(GLsizei, GLuint *);
using PFNDeleteFramebuffers = void(APIENTRY *)(GLsizei, const GLuint *);
using PFNBindFramebuffer = void(APIENTRY *)(GLenum, GLuint);
using PFNFramebufferTexture2D = void(APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFNCheckFramebufferStatus = GLenum(APIENTRY *)(GLenum);
using PFNBlitFramebuffer = void(APIENTRY *)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

// Reads and clears OpenGL's error flags; returns the first error, or GL_NO_ERROR. The
// host calls this before each of its checked calls, so a check reports only the errors of
// the host's own calls: ReShade, the add-on and librashader make OpenGL calls on the same
// context between the host's frames, and an error stays recorded until it is read.
// (OpenGL keeps at most one error per kind, so the loop is short.)
GLenum take_gl_error()
{
    GLenum first = GL_NO_ERROR;
    for (int i = 0; i < 16; ++i)
    {
        const GLenum e = glGetError();
        if (e == GL_NO_ERROR)
            break;
        if (first == GL_NO_ERROR)
            first = e;
    }
    return first;
}

// `code` in hexadecimal ("0x0502"), the form OpenGL's documentation uses.
std::string hex(GLenum code)
{
    char text[16];
    snprintf(text, sizeof(text), "0x%04X", unsigned(code));
    return text;
}

// `what` followed by " failed (OpenGL error 0x....)", for error messages.
std::string gl_failure(const char *what, GLenum code)
{
    return std::string(what) + " failed (OpenGL error " + hex(code) + ")";
}

// With OpenGL the back buffer is the window's client area, so the window is sized to the
// requested back buffer (see size_window in backend.h). Only Format::rgba8 is supported.
// See Backend in backend.h for what each public function does.
class OpenGL : public Backend
{
public:
    ~OpenGL() override
    {
        if (glrc_ != nullptr)
        {
            for (auto &[img, p] : pictures_)
            {
                glDeleteTextures(1, &p.tex);
                DeleteFramebuffers(1, &p.fbo);
            }
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(glrc_);
        }
        if (hdc_ != nullptr)
            ReleaseDC(o_.hwnd, hdc_);
    }

    bool init(const Options &o, std::string &error, bool &unsupported) override
    {
        o_ = o;
        if (o.format != Format::rgba8 || o.hdr10)
        {
            error = "the OpenGL host only draws 8-bit SDR back buffers";
            unsupported = true;
            return false;
        }
        if (!size_window(o.width, o.height))
        {
            error = "could not size the window";
            return false;
        }
        hdc_ = GetDC(o.hwnd);
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        const int pf = ChoosePixelFormat(hdc_, &pfd);
        if (pf == 0 || !SetPixelFormat(hdc_, pf, &pfd) || (glrc_ = wglCreateContext(hdc_)) == nullptr ||
            !wglMakeCurrent(hdc_, glrc_))
        {
            error = "could not create an OpenGL context";
            return false;
        }
        BindBuffer = reinterpret_cast<PFNBindBuffer>(wglGetProcAddress("glBindBuffer"));
        GenFramebuffers = reinterpret_cast<PFNGenFramebuffers>(wglGetProcAddress("glGenFramebuffers"));
        DeleteFramebuffers = reinterpret_cast<PFNDeleteFramebuffers>(wglGetProcAddress("glDeleteFramebuffers"));
        BindFramebuffer = reinterpret_cast<PFNBindFramebuffer>(wglGetProcAddress("glBindFramebuffer"));
        FramebufferTexture2D = reinterpret_cast<PFNFramebufferTexture2D>(wglGetProcAddress("glFramebufferTexture2D"));
        CheckFramebufferStatus = reinterpret_cast<PFNCheckFramebufferStatus>(wglGetProcAddress("glCheckFramebufferStatus"));
        BlitFramebuffer = reinterpret_cast<PFNBlitFramebuffer>(wglGetProcAddress("glBlitFramebuffer"));
        if (!BindBuffer || !GenFramebuffers || !DeleteFramebuffers || !BindFramebuffer || !FramebufferTexture2D ||
            !CheckFramebufferStatus || !BlitFramebuffer)
        {
            error = "OpenGL 3.0 buffer and framebuffer functions are not available";
            unsupported = true;
            return false;
        }
        return true;
    }

    bool resize(UINT w, UINT h, std::string &error) override
    {
        if (!size_window(w, h))
        {
            error = "could not resize the window";
            return false;
        }
        return true;
    }

    bool draw(const Image *img, std::string &error) override
    {
        take_gl_error(); // not this draw's: see take_gl_error
        BindFramebuffer(kDrawFramebuffer, 0);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, GLsizei(w_), GLsizei(h_));
        if (img == nullptr)
        {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            const GLenum e = take_gl_error();
            return e == GL_NO_ERROR || (error = gl_failure("glClear", e), false);
        }
        const Picture *p = picture(*img, error);
        if (p == nullptr)
            return false;
        // Texture row 0 is the picture's top row; window row 0 is the bottom. Flip, so
        // the window shows the picture the right way up, as the Direct3D hosts do.
        BindFramebuffer(kReadFramebuffer, p->fbo);
        BlitFramebuffer(0, 0, GLint(img->w), GLint(img->h), 0, GLint(img->h), GLint(img->w), 0, GL_COLOR_BUFFER_BIT,
                        GL_NEAREST);
        BindFramebuffer(kReadFramebuffer, 0);
        const GLenum e = take_gl_error();
        return e == GL_NO_ERROR || (error = gl_failure("glBlitFramebuffer", e), false);
    }

    bool read_back(Image &out, std::string &error) override
    {
        take_gl_error(); // not this read's: see take_gl_error
        std::vector<uint8_t> rows(size_t(w_) * h_ * 4);
        BindFramebuffer(kReadFramebuffer, 0);
        glReadBuffer(GL_BACK);
        // Rows are written to `rows`, tightly packed (no pixel buffer object bound).
        BindBuffer(kPixelPackBuffer, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glReadPixels(0, 0, GLsizei(w_), GLsizei(h_), GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        if (const GLenum e = take_gl_error(); e != GL_NO_ERROR)
        {
            error = gl_failure("glReadPixels", e);
            return false;
        }
        // Bottom row first: flip to top row first.
        std::vector<uint8_t> top_first(rows.size());
        const size_t pitch = size_t(w_) * 4;
        for (UINT y = 0; y < h_; ++y)
            std::memcpy(&top_first[size_t(y) * pitch], &rows[size_t(h_ - 1 - y) * pitch], pitch);
        decode(top_first.data(), pitch, w_, h_, Format::rgba8, false, out);
        return true;
    }

    bool present(std::string &error) override
    {
        if (!SwapBuffers(hdc_))
        {
            error = "SwapBuffers failed";
            return false;
        }
        return true;
    }

private:
    // A test picture on the GPU: the texture, and a framebuffer object with the texture
    // attached so glBlitFramebuffer can read from it.
    struct Picture
    {
        GLuint tex = 0, fbo = 0;
    };

    // Sizes the window's client area, and so the back buffer, to `w` x `h` and stores
    // that size. Returns false if the client area does not have that size afterwards.
    bool size_window(UINT w, UINT h)
    {
        if (!::size_window(o_.hwnd, w, h))
            return false;
        w_ = w;
        h_ = h;
        return true;
    }

    // Returns `img` uploaded as a texture with its framebuffer object, created on first use
    // and kept for the next frames. On failure, returns null and sets `error`.
    const Picture *picture(const Image &img, std::string &error)
    {
        if (auto it = pictures_.find(&img); it != pictures_.end())
            return &it->second;
        Picture p;
        glGenTextures(1, &p.tex);
        glBindTexture(GL_TEXTURE_2D, p.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // Pixels are read from img.rgba, tightly packed (no pixel buffer object bound).
        BindBuffer(kPixelUnpackBuffer, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(kRGBA8), GLsizei(img.w), GLsizei(img.h), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     img.rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        GLenum e = take_gl_error();
        if (e != GL_NO_ERROR)
            error = gl_failure("creating the picture texture", e);
        else
        {
            GenFramebuffers(1, &p.fbo);
            BindFramebuffer(kReadFramebuffer, p.fbo);
            FramebufferTexture2D(kReadFramebuffer, kColorAttachment0, GL_TEXTURE_2D, p.tex, 0);
            const GLenum status = CheckFramebufferStatus(kReadFramebuffer);
            BindFramebuffer(kReadFramebuffer, 0);
            if ((e = take_gl_error()) != GL_NO_ERROR)
                error = gl_failure("creating the picture's framebuffer", e);
            else if (status != kFramebufferComplete)
                error = "creating the picture's framebuffer failed (framebuffer status " + hex(status) + ")";
        }
        if (!error.empty())
        {
            glDeleteTextures(1, &p.tex);
            DeleteFramebuffers(1, &p.fbo);
            return nullptr;
        }
        return &(pictures_[&img] = p);
    }

    Options o_;
    HDC hdc_ = nullptr;
    HGLRC glrc_ = nullptr;
    UINT w_ = 0, h_ = 0; // back buffer (client area) size
    std::map<const Image *, Picture> pictures_;
    PFNBindBuffer BindBuffer = nullptr;
    PFNGenFramebuffers GenFramebuffers = nullptr;
    PFNDeleteFramebuffers DeleteFramebuffers = nullptr;
    PFNBindFramebuffer BindFramebuffer = nullptr;
    PFNFramebufferTexture2D FramebufferTexture2D = nullptr;
    PFNCheckFramebufferStatus CheckFramebufferStatus = nullptr;
    PFNBlitFramebuffer BlitFramebuffer = nullptr;
};
}

std::unique_ptr<Backend> make_opengl()
{
    return std::make_unique<OpenGL>();
}
