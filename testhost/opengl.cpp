#include "backend.h"

#include <GL/gl.h>

#include <map>

namespace
{
// OpenGL 3.0 framebuffer objects, loaded at run time (gl.h only covers OpenGL 1.1).
constexpr GLenum kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9, kColorAttachment0 = 0x8CE0,
                 kFramebufferComplete = 0x8CD5, kRGBA8 = 0x8058;
using PFNGenFramebuffers = void(APIENTRY *)(GLsizei, GLuint *);
using PFNDeleteFramebuffers = void(APIENTRY *)(GLsizei, const GLuint *);
using PFNBindFramebuffer = void(APIENTRY *)(GLenum, GLuint);
using PFNFramebufferTexture2D = void(APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFNCheckFramebufferStatus = GLenum(APIENTRY *)(GLenum);
using PFNBlitFramebuffer = void(APIENTRY *)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

// OpenGL draws into the window itself: its back buffer is exactly the window's client
// area. The window is therefore sized to the requested back buffer and kept at the
// bottom of the window stack, where it covers nothing the user is looking at.
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
        GenFramebuffers = reinterpret_cast<PFNGenFramebuffers>(wglGetProcAddress("glGenFramebuffers"));
        DeleteFramebuffers = reinterpret_cast<PFNDeleteFramebuffers>(wglGetProcAddress("glDeleteFramebuffers"));
        BindFramebuffer = reinterpret_cast<PFNBindFramebuffer>(wglGetProcAddress("glBindFramebuffer"));
        FramebufferTexture2D = reinterpret_cast<PFNFramebufferTexture2D>(wglGetProcAddress("glFramebufferTexture2D"));
        CheckFramebufferStatus = reinterpret_cast<PFNCheckFramebufferStatus>(wglGetProcAddress("glCheckFramebufferStatus"));
        BlitFramebuffer = reinterpret_cast<PFNBlitFramebuffer>(wglGetProcAddress("glBlitFramebuffer"));
        if (!GenFramebuffers || !DeleteFramebuffers || !BindFramebuffer || !FramebufferTexture2D || !CheckFramebufferStatus ||
            !BlitFramebuffer)
        {
            error = "OpenGL 3.0 framebuffer functions are not available";
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
        BindFramebuffer(kDrawFramebuffer, 0);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, GLsizei(w_), GLsizei(h_));
        if (img == nullptr)
        {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            return glGetError() == GL_NO_ERROR || (error = "glClear failed", false);
        }
        const Picture *p = picture(*img);
        if (p == nullptr)
        {
            error = "could not create the picture texture";
            return false;
        }
        // Texture row 0 is the picture's top row; window row 0 is the bottom. Flip, so
        // the window shows the picture the right way up, as the Direct3D hosts do.
        BindFramebuffer(kReadFramebuffer, p->fbo);
        BlitFramebuffer(0, 0, GLint(img->w), GLint(img->h), 0, GLint(img->h), GLint(img->w), 0, GL_COLOR_BUFFER_BIT,
                        GL_NEAREST);
        BindFramebuffer(kReadFramebuffer, 0);
        return glGetError() == GL_NO_ERROR || (error = "glBlitFramebuffer failed", false);
    }

    bool read_back(Image &out, std::string &error) override
    {
        std::vector<uint8_t> rows(size_t(w_) * h_ * 4);
        BindFramebuffer(kReadFramebuffer, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, GLsizei(w_), GLsizei(h_), GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
        if (glGetError() != GL_NO_ERROR)
        {
            error = "glReadPixels failed";
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
    struct Picture
    {
        GLuint tex = 0, fbo = 0;
    };

    bool size_window(UINT w, UINT h)
    {
        RECT r = {0, 0, LONG(w), LONG(h)};
        AdjustWindowRectEx(&r, DWORD(GetWindowLongW(o_.hwnd, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(o_.hwnd, GWL_EXSTYLE)));
        if (!SetWindowPos(o_.hwnd, HWND_BOTTOM, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE))
            return false;
        RECT c = {};
        GetClientRect(o_.hwnd, &c);
        w_ = UINT(c.right);
        h_ = UINT(c.bottom);
        return w_ == w && h_ == h;
    }

    const Picture *picture(const Image &img)
    {
        if (auto it = pictures_.find(&img); it != pictures_.end())
            return &it->second;
        Picture p;
        glGenTextures(1, &p.tex);
        glBindTexture(GL_TEXTURE_2D, p.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(kRGBA8), GLsizei(img.w), GLsizei(img.h), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     img.rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        GenFramebuffers(1, &p.fbo);
        BindFramebuffer(kReadFramebuffer, p.fbo);
        FramebufferTexture2D(kReadFramebuffer, kColorAttachment0, GL_TEXTURE_2D, p.tex, 0);
        const bool ok = CheckFramebufferStatus(kReadFramebuffer) == kFramebufferComplete && glGetError() == GL_NO_ERROR;
        BindFramebuffer(kReadFramebuffer, 0);
        if (!ok)
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
    UINT w_ = 0, h_ = 0;
    std::map<const Image *, Picture> pictures_;
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
