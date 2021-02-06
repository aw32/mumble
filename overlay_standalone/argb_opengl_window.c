// originally from here: http://stackoverflow.com/questions/9363491/how-to-make-transparent-window-on-linux
/*------------------------------------------------------------------------
     * A demonstration of OpenGL in a  ARGB window
     *    => support for composited window transparency
     *
     * (c) 2011 by Wolfgang 'datenwolf' Draxinger
     *     See me at comp.graphics.api.opengl and StackOverflow.com

     * License agreement: This source code is provided "as is". You
     * can use this source code however you want for your own personal
     * use. If you give this source code to anybody else then you must
     * leave this message in it.
     *
     * This program is based on the simplest possible
     * Linux OpenGL program by FTB (see info below)

      The simplest possible Linux OpenGL program? Maybe...

      (c) 2002 by FTB. See me in comp.graphics.api.opengl

      --
      <\___/>
      / O O \
      \_____/  FTB.

    ------------------------------------------------------------------------*/

    #include <stdbool.h>
    #include <stdlib.h>
    #include <stdio.h>
    #include <string.h>
    #include <math.h>
    #include <unistd.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <sys/un.h>
    #include <pwd.h>
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <poll.h>

    #include <GL/glew.h>
    #include <GL/glut.h>
    #include <GL/gl.h>
    #include <GL/glx.h>
    #include <GL/glxext.h>
    #include <X11/Xatom.h>
    #include <X11/extensions/Xrender.h>
    #include <X11/Xutil.h>

    #include "../overlay/overlay.h"

    #define USE_CHOOSE_FBCONFIG

    static void fatalError(const char *why)
    {
        fprintf(stderr, "%s", why);
        exit(0x666);
    }

    static int Xscreen;
    static Atom del_atom;
    static Colormap cmap;
    static Display *Xdisplay;
    static XVisualInfo *visual;
    static XRenderPictFormat *pict_format;
    static GLXFBConfig *fbconfigs, fbconfig;
    static int numfbconfigs;
    static GLXContext render_context;
    static Window Xroot, window_handle;
    static GLXWindow glX_window_handle;
    static int width, height;

    static int VisData[] = {
    GLX_RENDER_TYPE, GLX_RGBA_BIT,
    GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
    GLX_DOUBLEBUFFER, True,
    GLX_RED_SIZE, 8,
    GLX_GREEN_SIZE, 8,
    GLX_BLUE_SIZE, 8,
    GLX_ALPHA_SIZE, 8,
    GLX_DEPTH_SIZE, 16,
    None
    };


    static const char vshader[] = ""
							  "void main() {"
							  "gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;"
							  "gl_TexCoord[0] = gl_MultiTexCoord0;"
							  "}";

    static const char fshader[] = ""
							  "uniform sampler2D tex;"
							  "void main() {"
							  "gl_FragColor = texture2D(tex, gl_TexCoord[0].st);"
							  "}";

    const GLfloat fBorder[] = { 0.125f, 0.250f, 0.5f, 0.75f };


    
    typedef struct _Context {
	    unsigned int uiWidth, uiHeight;
    	unsigned int uiLeft, uiRight, uiTop, uiBottom;

    	struct sockaddr_un saName;
    	int iSocket;

	    // overlay message, temporary variable for processing from socket
    	struct OverlayMsg omMsg;
    	// opengl overlay texture
    	GLuint texture;

        // overlay texture in shared memory
        unsigned char *a_ucTexture;
        unsigned int uiMappedLength;

        bool bValid;
        bool bMesa;

        GLuint uiProgram;

        clock_t timeT;
        unsigned int frameCount;

        GLint maxVertexAttribs;
        GLboolean *vertexAttribStates;
       
    } Context;

    static Context ctx;

    static void releaseMem(Context *ctx) {
        if (ctx->a_ucTexture) {
            munmap(ctx->a_ucTexture, ctx->uiMappedLength);
            ctx->a_ucTexture    = NULL;
            ctx->uiMappedLength = 0;
        }
        if (ctx->texture != ~0U) {
            glDeleteTextures(1, &ctx->texture);
            ctx->texture = ~0U;
        }
        ctx->uiLeft = ctx->uiTop = ctx->uiRight = ctx->uiBottom = 0;
    }

    static void disconnect(Context *ctx) {
        releaseMem(ctx);
        ctx->uiWidth = ctx->uiHeight = 0;
        if (ctx->iSocket != -1) {
            close(ctx->iSocket);
            ctx->iSocket = -1;
        }
        printf("Disconnected\n");
    }

    static bool sendMessage(Context *ctx, struct OverlayMsg *om) {
        if (ctx->iSocket != -1) {
            size_t wantsend = sizeof(struct OverlayMsgHeader) + (size_t) om->omh.iLength;
            ssize_t sent    = send(ctx->iSocket, om, wantsend, MSG_DONTWAIT);
            if (sent != -1 && wantsend == (size_t) sent) {
                return true;
            }
            printf("Short write. Disconnecting pipe.\n");
        }
        disconnect(ctx);
        return false;
    }

    static void regenTexture(Context *ctx) {
        if (ctx->texture != ~0U) {
            glDeleteTextures(1, &ctx->texture);
        }
        glGenTextures(1, &ctx->texture);

        glBindTexture(GL_TEXTURE_2D, ctx->texture);
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, fBorder);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) ctx->uiWidth, (GLsizei) ctx->uiHeight, 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, ctx->a_ucTexture);
    }

    void init_mumble() {

        ctx.iSocket           = -1;
	    ctx.omMsg.omh.iLength = -1;
	    ctx.texture           = ~0U;
	    ctx.frameCount        = 0;
        char *home = getenv("HOME");
        if (home == NULL) {
            struct passwd *pwent = getpwuid(getuid());
            if (pwent && pwent->pw_dir && pwent->pw_dir[0]) {
                home = pwent->pw_dir;
            }
        }

        char *xdgRuntimeDir = getenv("XDG_RUNTIME_DIR");

        if (xdgRuntimeDir != NULL) {
            ctx.saName.sun_family = PF_UNIX;
            strcpy(ctx.saName.sun_path, xdgRuntimeDir);
            strcat(ctx.saName.sun_path, "/MumbleOverlayPipe");
        } else if (home) {
            ctx.saName.sun_family = PF_UNIX;
            strcpy(ctx.saName.sun_path, home);
            strcat(ctx.saName.sun_path, "/.MumbleOverlayPipe");
        }


        const char *vsource = vshader;
        const char *fsource = fshader;
        char buffer[8192];
        GLint l;
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(vs, 1, &vsource, NULL);
        glShaderSource(fs, 1, &fsource, NULL);
        glCompileShader(vs);
        glCompileShader(fs);
        glGetShaderInfoLog(vs, 8192, &l, buffer);
        printf("VERTEX: %s\n", buffer);
        glGetShaderInfoLog(fs, 8192, &l, buffer);
        printf("FRAGMENT: %s\n", buffer);
        ctx.uiProgram = glCreateProgram();
        glAttachShader(ctx.uiProgram, vs);
        glAttachShader(ctx.uiProgram, fs);
        glLinkProgram(ctx.uiProgram);


        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &ctx.maxVertexAttribs);
        ctx.vertexAttribStates = calloc((size_t) ctx.maxVertexAttribs, sizeof(GLboolean));

        // if no socket is active, initialize and connect to socket
        if (ctx.iSocket == -1) {
            releaseMem(&ctx);
            if (!ctx.saName.sun_path[0])
                return;
            ctx.iSocket = socket(AF_UNIX, SOCK_STREAM, 0);
            if (ctx.iSocket == -1) {
                printf("socket() failure\n");
                return;
            }
            fcntl(ctx.iSocket, F_SETFL, O_NONBLOCK, 1);
            if (connect(ctx.iSocket, (struct sockaddr *) (&ctx.saName), sizeof(ctx.saName)) != 0) {
                close(ctx.iSocket);
                ctx.iSocket = -1;
                printf("connect() failure %s\n", ctx.saName.sun_path);
                return;
            }
            printf("Socket connected\n");

            struct OverlayMsg om;
            om.omh.uiMagic = OVERLAY_MAGIC_NUMBER;
            om.omh.uiType  = OVERLAY_MSGTYPE_PID;
            om.omh.iLength = sizeof(struct OverlayMsgPid);
            om.omp.pid     = (unsigned int) getpid(); // getpid can't fail

            if (!sendMessage(&ctx, &om))
                return;

            printf("SentPid\n");
        }


        // if overlay size (width or height) is not up-to-date create and send an overlay initialization message
        if ((ctx.uiWidth != width) || (ctx.uiHeight != height)) {
            printf("Sending init overlay msg with w h %i %i\n", width, height);
            releaseMem(&ctx);

            ctx.uiWidth  = width;
            ctx.uiHeight = height;

            struct OverlayMsg om;
            om.omh.uiMagic  = OVERLAY_MAGIC_NUMBER;
            om.omh.uiType   = OVERLAY_MSGTYPE_INIT;
            om.omh.iLength  = sizeof(struct OverlayMsgInit);
            om.omi.uiWidth  = ctx.uiWidth;
            om.omi.uiHeight = ctx.uiHeight;

            if (!sendMessage(&ctx, &om))
                return;
        }


        GLenum err;
        while ((err = glGetError()) != GL_NO_ERROR) {
            printf("Init_mumble GLError: %p\n", err);
        }

    }

    static int read_mumble(Context *ctx) {
        int state = 0; // 0 nothing, 1 error, 2 update
        while(1) {
            printf("read_mumble_loop\n");
            if (ctx->omMsg.omh.iLength < 0) {
                // receive the overlay message header
                ssize_t length = recv(ctx->iSocket, ctx->omMsg.headerbuffer, sizeof(struct OverlayMsgHeader), 0);
                if (length < 0) {
                    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
                        return state;
                    disconnect(ctx);
                    return 1;
                } else if (length != sizeof(struct OverlayMsgHeader)) {
                    printf("Short header read on overlay message\n");
                    disconnect(ctx);
                    return 1;
                }
            } else {
                // receive the overlay message body
                ssize_t length = recv(ctx->iSocket, ctx->omMsg.msgbuffer, (size_t) ctx->omMsg.omh.iLength, 0);
                if (length < 0) {
                    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
                        return state;
                    disconnect(ctx);
                    return 1;
                } else if (length != ctx->omMsg.omh.iLength) {
                    printf("Short overlay message read %x %zd/%d\n", ctx->omMsg.omh.uiType, length, ctx->omMsg.omh.iLength);
                    disconnect(ctx);
                    return 1;
                }
                // set len to -1 again for a clean state on next receive
                ctx->omMsg.omh.iLength = -1;

                switch (ctx->omMsg.omh.uiType) {
                    // shared memory overlay message:
                    case OVERLAY_MSGTYPE_SHMEM: {
                        struct OverlayMsgShmem *oms = (struct OverlayMsgShmem *) &ctx->omMsg.omi;
                        printf("SHMEM %s\n", oms->a_cName);
                        releaseMem(ctx);
                        int fd = shm_open(oms->a_cName, O_RDONLY, 0600);
                        if (fd != -1) {
                            struct stat buf;

                            if (fstat(fd, &buf) != -1) {
                                unsigned int buflen = buf.st_size;
                                if (buflen >= ctx->uiWidth * ctx->uiHeight * 4 && buflen < 512 * 1024 * 1024) {
                                    ctx->uiMappedLength = buflen;
                                    ctx->a_ucTexture    = mmap(NULL, (size_t) buflen, PROT_READ, MAP_SHARED, fd, 0);
                                    if (ctx->a_ucTexture != MAP_FAILED) {
                                        // mmap successfull; send a new bodyless sharedmemory overlay message and regenerate
                                        // the overlay texture
                                        struct OverlayMsg om;
                                        om.omh.uiMagic = OVERLAY_MAGIC_NUMBER;
                                        om.omh.uiType  = OVERLAY_MSGTYPE_SHMEM;
                                        om.omh.iLength = 0;

                                        if (!sendMessage(ctx, &om)) {
                                            printf("sendMessage oops\n");
                                            return 1;
                                        }

                                        regenTexture(ctx);
                                        state = 2;
                                        continue;
                                    }
                                    ctx->a_ucTexture = NULL;
                                }
                                ctx->uiMappedLength = 0;
                            } else {
                                printf("Failed to fstat memory map\n");
                            }
                            close(fd);
                        }
                        printf("Failed to map memory\n");
                    } break;
                    // blit overlay message: blit overlay texture from shared memory to gl-texture var
                    case OVERLAY_MSGTYPE_BLIT: {
                        struct OverlayMsgBlit *omb = &ctx->omMsg.omb;
                        printf("BLIT %d %d %d %d\n", omb->x, omb->y, omb->w, omb->h);
                        if ((ctx->a_ucTexture != NULL) && (ctx->texture != ~0U)) {
                            glBindTexture(GL_TEXTURE_2D, ctx->texture);

                            if ((omb->x == 0) && (omb->y == 0) && (omb->w == ctx->uiWidth) && (omb->h == ctx->uiHeight)) {
                                printf("Optimzied fullscreen blit\n");
                                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) ctx->uiWidth, (GLsizei) ctx->uiHeight, 0,
                                             GL_BGRA, GL_UNSIGNED_BYTE, ctx->a_ucTexture);
                            } else {
                                // allocate temporary memory
                                unsigned int x     = omb->x;
                                unsigned int y     = omb->y;
                                unsigned int w     = omb->w;
                                unsigned int h     = omb->h;
                                unsigned char *ptr = (unsigned char *) malloc(w * h * 4);
                                unsigned int row;
                                memset(ptr, 0, w * h * 4);

                                // copy overlay texture to temporary memory to adapt to full opengl ui size (overlay at
                                // correct place)
                                for (row = 0; row < h; ++row) {
                                    const unsigned char *sptr = ctx->a_ucTexture + 4 * ((y + row) * ctx->uiWidth + x);
                                    unsigned char *dptr       = ptr + 4 * w * row;
                                    memcpy(dptr, sptr, w * 4);
                                }

                                // copy temporary texture to opengl
                                glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint) x, (GLint) y, (GLint) w, (GLint) h, GL_BGRA,
                                                GL_UNSIGNED_BYTE, ptr);
                                free(ptr);
                            }
                            state = 2;
                        }
                    } break;
                    case OVERLAY_MSGTYPE_ACTIVE: {
                        struct OverlayMsgActive *oma = &ctx->omMsg.oma;
                        printf("ACTIVE %d %d %d %d\n", oma->x, oma->y, oma->w, oma->h);
                        ctx->uiLeft   = oma->x;
                        ctx->uiTop    = oma->y;
                        ctx->uiRight  = oma->x + oma->w;
                        ctx->uiBottom = oma->y + oma->h;
                        state = 2;
                    } break;
                    case OVERLAY_MSGTYPE_INTERACTIVE: {
    #if defined(TARGET_MAC)
                        struct OverlayMsgInteractive *omin = &ctx->omMsg.omin;
                        printf("Interactive %d\n", omin->state);
                        if (bCursorAvail) {
                            if (omin->state) {
                                oCGDisplayHideCursor(kCGNullDirectDisplay);
                            } else {
                                oCGDisplayShowCursor(kCGNullDirectDisplay);
                            }
                        }
    #endif
                    } break;
                    default:
                        break;
                }
            }
        }
        return state;
    }

    static int drawOverlay(Context *ctx, unsigned int width, unsigned int height) {


        glBindTexture(GL_TEXTURE_2D, ctx->texture);
        glPushMatrix();

        float w = (float) (ctx->uiWidth);
        float h = (float) (ctx->uiHeight);

        float left   = (float) (ctx->uiLeft);
        float top    = (float) (ctx->uiTop);
        float right  = (float) (ctx->uiRight);
        float bottom = (float) (ctx->uiBottom);

        float xm  = left / w;
        float ym  = top / h;
        float xmx = right / w;
        float ymx = bottom / h;

        printf("draw w %f h %f left %f top %f right %f bottom %f\n", w, h, left, top, right, bottom);

        GLfloat vertex[] = { left, bottom, left,  top, right, top,

                             left, bottom, right, top, right, bottom };
        glVertexPointer(2, GL_FLOAT, 0, vertex);

        GLfloat tex[] = { xm, ymx, xm,  ym, xmx, ym,

                          xm, ymx, xmx, ym, xmx, ymx };
        glTexCoordPointer(2, GL_FLOAT, 0, tex);

        glDrawArrays(GL_TRIANGLES, 0, 6);

        glPopMatrix();

        //glClearColor(0.0, 0.0, 0.0, 0.0);
        //glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);


    }

    static void glerr(char *pref) {
        GLenum err;
        while ((err = glGetError()) != GL_NO_ERROR) {
            printf("%s GLError: %p\n", pref, err);
        }
    }

    static void redraw_mumble(Context *ctx, int width, int height) {
        GLuint program;
        GLint viewport[4];
        int i;

        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushClientAttrib(GL_ALL_ATTRIB_BITS);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, (GLint *) &program);

        glViewport(0, 0, width, height);

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, width, height, 0, -100.0, 100.0);

        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        glMatrixMode(GL_TEXTURE);
        glPushMatrix();
        glLoadIdentity();

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glDisable(GL_ALPHA_TEST);
        glDisable(GL_AUTO_NORMAL);
        // Skip clip planes, there are thousands of them.
        glDisable(GL_COLOR_LOGIC_OP);
        glDisable(GL_COLOR_TABLE);
        glDisable(GL_CONVOLUTION_1D);
        glDisable(GL_CONVOLUTION_2D);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_DITHER);
        glDisable(GL_FOG);
        glDisable(GL_HISTOGRAM);
        glDisable(GL_INDEX_LOGIC_OP);
        glDisable(GL_LIGHTING);
        glDisable(GL_NORMALIZE);
        // Skip line smmooth
        // Skip map
        glDisable(GL_MINMAX);
        // Skip polygon offset
        glDisable(GL_SEPARABLE_2D);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);

        GLboolean b = 0;
        glGetBooleanv(GL_TEXTURE_GEN_Q, &b);
        if (b)
            glDisable(GL_TEXTURE_GEN_Q);
        glGetBooleanv(GL_TEXTURE_GEN_R, &b);
        if (b)
            glDisable(GL_TEXTURE_GEN_R);
        glGetBooleanv(GL_TEXTURE_GEN_S, &b);
        if (b)
            glDisable(GL_TEXTURE_GEN_S);
        glGetBooleanv(GL_TEXTURE_GEN_T, &b);
        if (b)
            glDisable(GL_TEXTURE_GEN_T);

        glRenderMode(GL_RENDER);

        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_INDEX_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_EDGE_FLAG_ARRAY);

        glPixelStorei(GL_UNPACK_SWAP_BYTES, 0);
        glPixelStorei(GL_UNPACK_LSB_FIRST, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        GLint texunits = 1;

        glGetIntegerv(GL_MAX_TEXTURE_UNITS, &texunits);

        for (i = texunits - 1; i >= 0; --i) {
            glActiveTexture(GL_TEXTURE0 + (GLenum) i);
            glDisable(GL_TEXTURE_1D);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_TEXTURE_3D);
        }

        glDisable(GL_TEXTURE_CUBE_MAP);
        glDisable(GL_VERTEX_PROGRAM_ARB);
        glDisable(GL_FRAGMENT_PROGRAM_ARB);

        GLint enabled;
        for (i = 0; i < ctx->maxVertexAttribs; ++i) {
            enabled = GL_FALSE;
            glGetVertexAttribiv((GLuint) i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
            if (enabled == GL_TRUE) {
                glDisableVertexAttribArray((GLuint) i);
                ctx->vertexAttribStates[i] = GL_TRUE;
            }
        }

        glUseProgram(ctx->uiProgram);

        glEnable(GL_COLOR_MATERIAL);
        glEnable(GL_TEXTURE_2D);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

        glMatrixMode(GL_MODELVIEW);

        GLint uni = glGetUniformLocation(ctx->uiProgram, "tex");
        glUniform1i(uni, 0);

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);

        GLuint bound = 0, vbobound = 0;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, (GLint *) &bound);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, (GLint *) &vbobound);

        if (bound != 0) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
        }
        if (vbobound != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }


        drawOverlay(ctx, (unsigned int) width, (unsigned int) height);

        if (bound != 0) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, bound);
        }
        if (vbobound != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, vbobound);
        }

        for (i = 0; i < ctx->maxVertexAttribs; ++i) {
            if (ctx->vertexAttribStates[i] == GL_TRUE) {
                glEnableVertexAttribArray((GLuint) i);
                ctx->vertexAttribStates[i] = GL_FALSE;
            }
        }

        glMatrixMode(GL_TEXTURE);
        glPopMatrix();

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();

        glMatrixMode(GL_PROJECTION);
        glPopMatrix();

        glPopClientAttrib();
        glPopAttrib();
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glUseProgram(program);

        // drain opengl error queue
        GLenum err;
        while ((err = glGetError()) != GL_NO_ERROR) {
            //printf("GLError: %p\n", err);
            ;
        }

    }


    static int isExtensionSupported(const char *extList, const char *extension)
    {

      const char *start;
      const char *where, *terminator;

      /* Extension names should not have spaces. */
      where = strchr(extension, ' ');
      if ( where || *extension == '\0' )
        return 0;

      /* It takes a bit of care to be fool-proof about parsing the
         OpenGL extensions string. Don't be fooled by sub-strings,
         etc. */
      for ( start = extList; ; ) {
        where = strstr( start, extension );

        if ( !where )
          break;

        terminator = where + strlen( extension );

        if ( where == start || *(where - 1) == ' ' )
          if ( *terminator == ' ' || *terminator == '\0' )
        return 1;

        start = terminator;
      }
      return 0;
    }

    static Bool WaitForMapNotify(Display *d, XEvent *e, char *arg)
    {
        return d && e && arg && (e->type == MapNotify) && (e->xmap.window == *(Window*)arg);
    }

    static void describe_fbconfig(GLXFBConfig fbconfig)
    {
        int doublebuffer;
        int red_bits, green_bits, blue_bits, alpha_bits, depth_bits;

        glXGetFBConfigAttrib(Xdisplay, fbconfig, GLX_DOUBLEBUFFER, &doublebuffer);
        glXGetFBConfigAttrib(Xdisplay, fbconfig, GLX_RED_SIZE, &red_bits);
        glXGetFBConfigAttrib(Xdisplay, fbconfig, GLX_GREEN_SIZE, &green_bits);
        glXGetFBConfigAttrib(Xdisplay, fbconfig, GLX_BLUE_SIZE, &blue_bits);
        glXGetFBConfigAttrib(Xdisplay, fbconfig, GLX_ALPHA_SIZE, &alpha_bits);
        glXGetFBConfigAttrib(Xdisplay, fbconfig, GLX_DEPTH_SIZE, &depth_bits);

        fprintf(stderr, "FBConfig selected:\n"
        "Doublebuffer: %s\n"
        "Red Bits: %d, Green Bits: %d, Blue Bits: %d, Alpha Bits: %d, Depth Bits: %d\n",
        doublebuffer == True ? "Yes" : "No",
        red_bits, green_bits, blue_bits, alpha_bits, depth_bits);
    }

    static void createTheWindow()
    {
        XEvent event;
        int x,y, attr_mask;
        XSizeHints hints;
        XWMHints *startup_state;
        XTextProperty textprop;
        XSetWindowAttributes attr = {0,};
        static char *title = "FTB's little OpenGL example - ARGB extension by WXD";

        Xdisplay = XOpenDisplay(NULL);
        if (!Xdisplay) {
        fatalError("Couldn't connect to X server\n");
        }
        Xscreen = DefaultScreen(Xdisplay);
        Xroot = RootWindow(Xdisplay, Xscreen);

        fbconfigs = glXChooseFBConfig(Xdisplay, Xscreen, VisData, &numfbconfigs);
        fbconfig = 0;
        for(int i = 0; i<numfbconfigs; i++) {
        visual = (XVisualInfo*) glXGetVisualFromFBConfig(Xdisplay, fbconfigs[i]);
        if(!visual)
            continue;

        pict_format = XRenderFindVisualFormat(Xdisplay, visual->visual);
        if(!pict_format)
            continue;

        fbconfig = fbconfigs[i];
        if(pict_format->direct.alphaMask > 0) {
            break;
        }
        }

        if(!fbconfig) {
        fatalError("No matching FB config found");
        }

        describe_fbconfig(fbconfig);

        /* Create a colormap - only needed on some X clients, eg. IRIX */
        cmap = XCreateColormap(Xdisplay, Xroot, visual->visual, AllocNone);

        attr.colormap = cmap;
        attr.background_pixmap = None;
        attr.border_pixmap = None;
        attr.border_pixel = 0;
        attr.event_mask =
        StructureNotifyMask |
        EnterWindowMask |
        LeaveWindowMask |
        ExposureMask |
        ButtonPressMask |
        ButtonReleaseMask |
        OwnerGrabButtonMask |
        KeyPressMask |
        KeyReleaseMask;

        attr_mask =
        CWBackPixmap|
        CWColormap|
        CWBorderPixel|
        CWEventMask;

        width = DisplayWidth(Xdisplay, DefaultScreen(Xdisplay))/2;
        height = DisplayHeight(Xdisplay, DefaultScreen(Xdisplay))/2;
        x=width/2, y=height/2;

        window_handle = XCreateWindow(  Xdisplay,
                Xroot,
                x, y, width, height,
                0,
                visual->depth,
                InputOutput,
                visual->visual,
                attr_mask, &attr);

        if( !window_handle ) {
        fatalError("Couldn't create the window\n");
        }

    #if USE_GLX_CREATE_WINDOW
        int glXattr[] = { None };
        glX_window_handle = glXCreateWindow(Xdisplay, fbconfig, window_handle, glXattr);
        if( !glX_window_handle ) {
        fatalError("Couldn't create the GLX window\n");
        }
    #else
        glX_window_handle = window_handle;
    #endif

        textprop.value = (unsigned char*)title;
        textprop.encoding = XA_STRING;
        textprop.format = 8;
        textprop.nitems = strlen(title);

        hints.x = x;
        hints.y = y;
        hints.width = width;
        hints.height = height;
        hints.flags = USPosition|USSize;

        startup_state = XAllocWMHints();
        startup_state->initial_state = NormalState;
        startup_state->flags = StateHint;

        XSetWMProperties(Xdisplay, window_handle,&textprop, &textprop,
            NULL, 0,
            &hints,
            startup_state,
            NULL);


        XFree(startup_state);

        XMapWindow(Xdisplay, window_handle);
        XIfEvent(Xdisplay, &event, WaitForMapNotify, (char*)&window_handle);

        if ((del_atom = XInternAtom(Xdisplay, "WM_DELETE_WINDOW", 0)) != None) {
        XSetWMProtocols(Xdisplay, window_handle, &del_atom, 1);
        }
    }

    static int ctxErrorHandler( Display *dpy, XErrorEvent *ev )
    {
        fputs("Error at context creation", stderr);
        return 0;
    }

    static void createTheRenderContext()
    {
        int dummy;
        if (!glXQueryExtension(Xdisplay, &dummy, &dummy)) {
        fatalError("OpenGL not supported by X server\n");
        }

    #if USE_GLX_CREATE_CONTEXT_ATTRIB
        #define GLX_CONTEXT_MAJOR_VERSION_ARB       0x2091
        #define GLX_CONTEXT_MINOR_VERSION_ARB       0x2092
        render_context = NULL;
        if( isExtensionSupported( glXQueryExtensionsString(Xdisplay, DefaultScreen(Xdisplay)), "GLX_ARB_create_context" ) ) {
        typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
        glXCreateContextAttribsARBProc glXCreateContextAttribsARB = (glXCreateContextAttribsARBProc)glXGetProcAddressARB( (const GLubyte *) "glXCreateContextAttribsARB" );
        if( glXCreateContextAttribsARB ) {
            int context_attribs[] =
            {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 0,
            //GLX_CONTEXT_FLAGS_ARB        , GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
            None
            };

            int (*oldHandler)(Display*, XErrorEvent*) = XSetErrorHandler(&ctxErrorHandler);

            render_context = glXCreateContextAttribsARB( Xdisplay, fbconfig, 0, True, context_attribs );

            XSync( Xdisplay, False );
            XSetErrorHandler( oldHandler );

            fputs("glXCreateContextAttribsARB failed", stderr);
        } else {
            fputs("glXCreateContextAttribsARB could not be retrieved", stderr);
        }
        } else {
            fputs("glXCreateContextAttribsARB not supported", stderr);
        }

        if(!render_context)
        {
    #else
        {
    #endif
        render_context = glXCreateNewContext(Xdisplay, fbconfig, GLX_RGBA_TYPE, 0, True);
        if (!render_context) {
            fatalError("Failed to create a GL context\n");
        }
        }

        if (!glXMakeContextCurrent(Xdisplay, glX_window_handle, glX_window_handle, render_context)) {
        fatalError("glXMakeCurrent failed for window\n");
        }
    }

    static int updateTheMessageQueue()
    {
        XEvent event;
        XConfigureEvent *xc;

        int state = 1;

        while (XPending(Xdisplay))
        {
            XNextEvent(Xdisplay, &event);
            printf("event.type %d\n", event.type);
            switch (event.type)
            {
                case ClientMessage:
                    if (event.xclient.data.l[0] == del_atom)
                    {
                        return 0;
                    }
                break;

                case ConfigureNotify:
                    xc = &(event.xconfigure);
                    width = xc->width;
                    height = xc->height;
                    state = 2;
                break;
            }
        }
        return state;
    }

    /*  6----7
       /|   /|
      3----2 |
      | 5--|-4
      |/   |/
      0----1

    */

    GLfloat cube_vertices[][8] =  {
        /*  X     Y     Z   Nx   Ny   Nz    S    T */
        {-1.0, -1.0,  1.0, 0.0, 0.0, 1.0, 0.0, 0.0}, // 0
        { 1.0, -1.0,  1.0, 0.0, 0.0, 1.0, 1.0, 0.0}, // 1
        { 1.0,  1.0,  1.0, 0.0, 0.0, 1.0, 1.0, 1.0}, // 2
        {-1.0,  1.0,  1.0, 0.0, 0.0, 1.0, 0.0, 1.0}, // 3

        { 1.0, -1.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0}, // 4
        {-1.0, -1.0, -1.0, 0.0, 0.0, -1.0, 1.0, 0.0}, // 5
        {-1.0,  1.0, -1.0, 0.0, 0.0, -1.0, 1.0, 1.0}, // 6
        { 1.0,  1.0, -1.0, 0.0, 0.0, -1.0, 0.0, 1.0}, // 7

        {-1.0, -1.0, -1.0, -1.0, 0.0, 0.0, 0.0, 0.0}, // 5
        {-1.0, -1.0,  1.0, -1.0, 0.0, 0.0, 1.0, 0.0}, // 0
        {-1.0,  1.0,  1.0, -1.0, 0.0, 0.0, 1.0, 1.0}, // 3
        {-1.0,  1.0, -1.0, -1.0, 0.0, 0.0, 0.0, 1.0}, // 6

        { 1.0, -1.0,  1.0,  1.0, 0.0, 0.0, 0.0, 0.0}, // 1
        { 1.0, -1.0, -1.0,  1.0, 0.0, 0.0, 1.0, 0.0}, // 4
        { 1.0,  1.0, -1.0,  1.0, 0.0, 0.0, 1.0, 1.0}, // 7
        { 1.0,  1.0,  1.0,  1.0, 0.0, 0.0, 0.0, 1.0}, // 2

        {-1.0, -1.0, -1.0,  0.0, -1.0, 0.0, 0.0, 0.0}, // 5
        { 1.0, -1.0, -1.0,  0.0, -1.0, 0.0, 1.0, 0.0}, // 4
        { 1.0, -1.0,  1.0,  0.0, -1.0, 0.0, 1.0, 1.0}, // 1
        {-1.0, -1.0,  1.0,  0.0, -1.0, 0.0, 0.0, 1.0}, // 0

        {-1.0, 1.0,  1.0,  0.0,  1.0, 0.0, 0.0, 0.0}, // 3
        { 1.0, 1.0,  1.0,  0.0,  1.0, 0.0, 1.0, 0.0}, // 2
        { 1.0, 1.0, -1.0,  0.0,  1.0, 0.0, 1.0, 1.0}, // 7
        {-1.0, 1.0, -1.0,  0.0,  1.0, 0.0, 0.0, 1.0}, // 6
    };

    static void draw_cube(void)
    {
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);

        glVertexPointer(3, GL_FLOAT, sizeof(GLfloat) * 8, &cube_vertices[0][0]);
        glNormalPointer(GL_FLOAT, sizeof(GLfloat) * 8, &cube_vertices[0][3]);
        glTexCoordPointer(2, GL_FLOAT, sizeof(GLfloat) * 8, &cube_vertices[0][6]);

        glDrawArrays(GL_QUADS, 0, 24);
    }

    float const light0_dir[]={0,1,0,0};
    float const light0_color[]={78./255., 80./255., 184./255.,1};

    float const light1_dir[]={-1,1,1,0};
    float const light1_color[]={255./255., 220./255., 97./255.,1};

    float const light2_dir[]={0,-1,0,0};
    float const light2_color[]={31./255., 75./255., 16./255.,1};

    static void redrawTheWindow()
    {
        float const aspect = (float)width / (float)height;

        static float a=0;
        static float b=0;
        static float c=0;

        glDrawBuffer(GL_BACK);

        glViewport(0, 0, width, height);

        // Clear with alpha = 0.0, i.e. full transparency
        glClearColor(0.0, 0.0, 0.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-aspect, aspect, -1, 1, 2.5, 10);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glLightfv(GL_LIGHT0, GL_POSITION, light0_dir);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, light0_color);

        glLightfv(GL_LIGHT1, GL_POSITION, light1_dir);
        glLightfv(GL_LIGHT1, GL_DIFFUSE, light1_color);

        glLightfv(GL_LIGHT2, GL_POSITION, light2_dir);
        glLightfv(GL_LIGHT2, GL_DIFFUSE, light2_color);

        glTranslatef(0., 0., -5.);

        glRotatef(a, 1, 0, 0);
        glRotatef(b, 0, 1, 0);
        glRotatef(c, 0, 0, 1);

        glEnable(GL_LIGHT0);
        glEnable(GL_LIGHT1);
        glEnable(GL_LIGHTING);

        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

        glColor4f(1., 1., 1., 0.5);

        glCullFace(GL_FRONT);
        draw_cube();
        glCullFace(GL_BACK);
        draw_cube();

        a = fmod(a+0.1, 360.);
        b = fmod(b+0.5, 360.);
        c = fmod(c+0.25, 360.);


        redraw_mumble(&ctx, width, height);

        glXSwapBuffers(Xdisplay, glX_window_handle);
    }

    int main(int argc, char *argv[])
    {
        createTheWindow();
        createTheRenderContext();

        //glutInit(&argc, argv);
        GLenum err = glewInit();
        if (GLEW_OK != err)
        {
          /* Problem: glewInit failed, something is seriously wrong. */
          fprintf(stderr, "Error: %s\n", glewGetErrorString(err));
          return 1;
        }
        fprintf(stdout, "Status: Using GLEW %s\n", glewGetString(GLEW_VERSION));


        init_mumble();

        int xmsg = 0;

        int xfd = ConnectionNumber(Xdisplay);
        int mfd = ctx.iSocket;

        struct pollfd pfd[2];
        memset(&pfd, 0, sizeof(pfd));
        pfd[0].fd = xfd;
        pfd[0].events = POLLIN;
        pfd[1].fd = mfd;
        pfd[1].events = POLLIN;
        int poll_ret = 0;

        while(1) {

            poll_ret = poll(pfd, 2, -1);

            xmsg = updateTheMessageQueue();
            if (xmsg == 0) {
                break;
            }

            printf("xmsg %d\n", xmsg);

            int msg = 0;
            msg = read_mumble(&ctx);
            if (msg != 0) {
                printf("MSG: %d\n", msg);
            }
            if (xmsg == 2 || msg == 2) {
                redrawTheWindow();
            }
            if (poll_ret == -1) {
                break;
            }
        }

        return 0;
    }
