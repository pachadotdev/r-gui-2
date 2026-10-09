#include <R.h>
#include <Rinternals.h>
#include <R_ext/GraphicsEngine.h>
#include <R_ext/GraphicsDevice.h>
#include <R_ext/Rdynload.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#else
#include <winsock2.h>
#endif

#ifndef RGUI2_NO_CAIRO
#include <cairo/cairo.h>
#endif

typedef struct {
    int width, height;
    char *port_file;
    pDevDesc dd;
#ifndef RGUI2_NO_CAIRO
    cairo_surface_t *surface;
    cairo_t *cr;
#endif
} Rgui2Device;

static Rgui2Device *active_device = NULL;

#ifndef RGUI2_NO_CAIRO
static Rgui2Device *state(pDevDesc dd) { return (Rgui2Device *) dd->deviceSpecific; }

static void colour(cairo_t *cr, int c) {
    if (c == NA_INTEGER || R_ALPHA(c) == 0) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        return;
    }
    cairo_set_source_rgba(cr, R_RED(c) / 255.0, R_GREEN(c) / 255.0,
                          R_BLUE(c) / 255.0, R_ALPHA(c) / 255.0);
}

static void setup_context(cairo_t *cr, const pGEcontext gc) {
    cairo_set_line_width(cr, fmax(0.1, gc->lwd));
    cairo_set_line_cap(cr, gc->lend == GE_ROUND_CAP ? CAIRO_LINE_CAP_ROUND :
                       gc->lend == GE_SQUARE_CAP ? CAIRO_LINE_CAP_SQUARE :
                       CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, gc->ljoin == GE_ROUND_JOIN ? CAIRO_LINE_JOIN_ROUND :
                        gc->ljoin == GE_BEVEL_JOIN ? CAIRO_LINE_JOIN_BEVEL :
                        CAIRO_LINE_JOIN_MITER);
}

static void path_points(cairo_t *cr, int n, double *x, double *y, int h) {
    if (n < 1) return;
    cairo_move_to(cr, x[0], h - y[0]);
    for (int i = 1; i < n; ++i) cairo_line_to(cr, x[i], h - y[i]);
}

static void send_frame(Rgui2Device *d) {
    if (!d || !d->port_file || !d->surface) return;
    FILE *pf = fopen(d->port_file, "r");
    if (!pf) return;
    int port = 0;
    if (fscanf(pf, "%d", &port) != 1 || port <= 0) { fclose(pf); return; }
    fclose(pf);
    cairo_surface_flush(d->surface);
    unsigned char *data = cairo_image_surface_get_data(d->surface);
    int fd = -1;
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return;
    fd = (int)s;
#else
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket((SOCKET)fd);
#else
        close(fd);
#endif
        return;
    }
    char header[128];
    int hn = snprintf(header, sizeof(header), "RGPL %d %d\n", d->width, d->height);
#ifdef _WIN32
    send((SOCKET)fd, header, hn, 0);
#else
    (void)!write(fd, header, (size_t)hn);
#endif
    unsigned char *rgb = (unsigned char *)malloc((size_t)d->width*d->height*3);
    if (rgb) {
        const int stride = cairo_image_surface_get_stride(d->surface);
        for (int y=0; y<d->height; ++y) for (int x=0; x<d->width; ++x) {
            unsigned char *p = data + (size_t)y * stride + 4*(size_t)x;
            unsigned char *q = rgb + 3*((size_t)y*d->width+x);
            /*
             * Cairo stores premultiplied ARGB32 in native byte order.  The
             * Qt transport is straight RGB, so account for both the row
             * stride and premultiplied colour channels here.
             */
            const unsigned int alpha = p[3];
            if (alpha == 0) {
                q[0] = q[1] = q[2] = 255;
            } else {
                q[0] = (unsigned char)((p[2] * 255U + alpha / 2U) / alpha);
                q[1] = (unsigned char)((p[1] * 255U + alpha / 2U) / alpha);
                q[2] = (unsigned char)((p[0] * 255U + alpha / 2U) / alpha);
            }
        }
        size_t left=(size_t)d->width*d->height*3, off=0;
        while (left) {
#ifdef _WIN32
            int n=send((SOCKET)fd,(const char*)rgb+off,(int)(left>INT_MAX?INT_MAX:left),0);
#else
            ssize_t n=write(fd,rgb+off,left);
#endif
            if (n<=0) break; off+=(size_t)n; left-=(size_t)n;
        }
        free(rgb);
    }
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

static void activate(const pDevDesc dd) { (void)dd; }
static void deactivate(pDevDesc dd) { (void)dd; }
static void mode(int mode, pDevDesc dd) {
    (void)mode; (void)dd;
}
static void new_page(const pGEcontext gc, pDevDesc dd) {
    Rgui2Device *d = state(dd);
    cairo_save(d->cr);
    cairo_set_operator(d->cr, CAIRO_OPERATOR_SOURCE);
    colour(d->cr, gc->fill == NA_INTEGER ? R_RGB(255,255,255) : gc->fill);
    cairo_paint(d->cr);
    cairo_restore(d->cr);
}
static void line(double x1, double y1, double x2, double y2,
                 const pGEcontext gc, pDevDesc dd) {
    Rgui2Device *d = state(dd); setup_context(d->cr, gc); colour(d->cr, gc->col);
    cairo_move_to(d->cr, x1, d->height-y1); cairo_line_to(d->cr, x2, d->height-y2);
    cairo_stroke(d->cr);
}
static void polyline(int n, double *x, double *y, const pGEcontext gc, pDevDesc dd) {
    Rgui2Device *d = state(dd); setup_context(d->cr, gc); colour(d->cr, gc->col);
    path_points(d->cr, n, x, y, d->height); cairo_stroke(d->cr);
}
static void polygon(int n, double *x, double *y, const pGEcontext gc, pDevDesc dd) {
    Rgui2Device *d = state(dd); setup_context(d->cr, gc);
    path_points(d->cr, n, x, y, d->height); cairo_close_path(d->cr);
    if (gc->fill != NA_INTEGER && R_ALPHA(gc->fill)) { colour(d->cr, gc->fill); cairo_fill_preserve(d->cr); }
    if (gc->col != NA_INTEGER && R_ALPHA(gc->col)) { colour(d->cr, gc->col); cairo_stroke(d->cr); }
    else cairo_new_path(d->cr);
}
static void rect(double x0, double y0, double x1, double y1,
                 const pGEcontext gc, pDevDesc dd) {
    Rgui2Device *d = state(dd); setup_context(d->cr, gc);
    double left = fmin(x0,x1), bottom = fmin(y0,y1);
    cairo_rectangle(d->cr, left, d->height-fmax(y0,y1), fabs(x1-x0), fabs(y1-y0));
    if (gc->fill != NA_INTEGER && R_ALPHA(gc->fill)) { colour(d->cr, gc->fill); cairo_fill_preserve(d->cr); }
    if (gc->col != NA_INTEGER && R_ALPHA(gc->col)) { colour(d->cr, gc->col); cairo_stroke(d->cr); }
    else cairo_new_path(d->cr);
    (void)bottom;
}
static void circle(double x, double y, double r, const pGEcontext gc, pDevDesc dd) {
    Rgui2Device *d = state(dd); setup_context(d->cr, gc);
    cairo_arc(d->cr, x, d->height-y, r, 0, 2*M_PI);
    if (gc->fill != NA_INTEGER && R_ALPHA(gc->fill)) { colour(d->cr, gc->fill); cairo_fill_preserve(d->cr); }
    if (gc->col != NA_INTEGER && R_ALPHA(gc->col)) { colour(d->cr, gc->col); cairo_stroke(d->cr); }
    else cairo_new_path(d->cr);
}
static void path(double *x, double *y, int npoly, int *nper, Rboolean winding,
                 const pGEcontext gc, pDevDesc dd) {
    Rgui2Device *d = state(dd); setup_context(d->cr, gc);
    int off = 0;
    for (int p=0; p<npoly; ++p) {
        path_points(d->cr, nper[p], x+off, y+off, d->height);
        cairo_close_path(d->cr); off += nper[p];
    }
    cairo_set_fill_rule(d->cr, winding ? CAIRO_FILL_RULE_WINDING : CAIRO_FILL_RULE_EVEN_ODD);
    if (gc->fill != NA_INTEGER && R_ALPHA(gc->fill)) { colour(d->cr, gc->fill); cairo_fill_preserve(d->cr); }
    if (gc->col != NA_INTEGER && R_ALPHA(gc->col)) { colour(d->cr, gc->col); cairo_stroke(d->cr); }
    else cairo_new_path(d->cr);
}
static void raster(unsigned int *raster, int w, int h, double x, double y,
                   double width, double height, double rot, Rboolean interpolate,
                   const pGEcontext gc, pDevDesc dd) {
    Rgui2Device *d = state(dd);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);
    unsigned char *dst = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    for (int j=0;j<h;j++) for (int i=0;i<w;i++) {
        unsigned int c=raster[j*w+i];
        unsigned char a=R_ALPHA(c), *q=dst+(size_t)j*stride+4*(size_t)i;
        q[0]=(unsigned char)(R_BLUE(c)*a/255); q[1]=(unsigned char)(R_GREEN(c)*a/255);
        q[2]=(unsigned char)(R_RED(c)*a/255); q[3]=a;
    }
    cairo_surface_mark_dirty(s); cairo_save(d->cr);
    cairo_translate(d->cr,x,d->height-y); cairo_rotate(d->cr,-rot*M_PI/180.0);
    cairo_scale(d->cr,width/w,height/h); cairo_set_source_surface(d->cr,s,0,-h);
    cairo_paint(d->cr); cairo_restore(d->cr); cairo_surface_destroy(s); (void)gc;(void)interpolate;
}
static void text_impl(double x,double y,const char *str,double rot,double hadj,
                      const pGEcontext gc,pDevDesc dd) {
    Rgui2Device *d=state(dd); cairo_save(d->cr);
    cairo_select_font_face(d->cr, gc->fontfamily[0]?gc->fontfamily:"sans",
        gc->fontface==3?CAIRO_FONT_SLANT_ITALIC:CAIRO_FONT_SLANT_NORMAL,
        gc->fontface==2?CAIRO_FONT_WEIGHT_BOLD:CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(d->cr, gc->ps*gc->cex/72.0*96.0);
    cairo_text_extents_t e; cairo_text_extents(d->cr,str,&e);
    cairo_translate(d->cr,x,d->height-y); cairo_rotate(d->cr,-rot*M_PI/180.0);
    cairo_move_to(d->cr,-hadj*e.x_advance,0); colour(d->cr,gc->col); cairo_show_text(d->cr,str);
    cairo_restore(d->cr);
}
static void text(double x,double y,const char *s,double r,double a,const pGEcontext g,pDevDesc d){text_impl(x,y,s,r,a,g,d);}
static void text_utf8(double x,double y,const char *s,double r,double a,const pGEcontext g,pDevDesc d){text_impl(x,y,s,r,a,g,d);}
static double str_width(const char *s,const pGEcontext g,pDevDesc dd) {
    Rgui2Device *d=state(dd); cairo_select_font_face(d->cr,g->fontfamily[0]?g->fontfamily:"sans",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(d->cr,g->ps*g->cex/72.0*96.0); cairo_text_extents_t e; cairo_text_extents(d->cr,s,&e); return e.x_advance;
}
static double str_width_utf8(const char *s,const pGEcontext g,pDevDesc d){return str_width(s,g,d);}
static void metric(int c,const pGEcontext g,double *a,double *des,double *w,pDevDesc dd) {
    char s[8]; int n=0; if(c<0)c=-c; if(c<128)s[n++]=(char)c; else s[n++]='M'; s[n]=0;
    Rgui2Device *d=state(dd); cairo_select_font_face(d->cr,g->fontfamily[0]?g->fontfamily:"sans",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(d->cr,g->ps*g->cex/72.0*96.0); cairo_font_extents_t e; cairo_font_extents(d->cr,&e);
    cairo_text_extents_t te; cairo_text_extents(d->cr,s,&te); *a=e.ascent; *des=e.descent; *w=te.x_advance;
}
static void clip(double x0,double x1,double y0,double y1,pDevDesc dd) {
    Rgui2Device *d=state(dd); cairo_reset_clip(d->cr); cairo_rectangle(d->cr,x0,d->height-y1,x1-x0,y1-y0); cairo_clip(d->cr);
}
static void size(double *l,double *r,double *b,double *t,pDevDesc dd){Rgui2Device*d=state(dd);*l=0;*r=d->width;*b=0;*t=d->height;}
static void close_device(pDevDesc dd){Rgui2Device*d=state(dd);if(active_device==d)active_device=NULL;cairo_destroy(d->cr);cairo_surface_destroy(d->surface);free(d->port_file);free(d);}

static void init_callbacks(pDevDesc dd) {
    dd->activate=activate; dd->deactivate=deactivate; dd->mode=mode; dd->newPage=new_page;
    dd->line=line; dd->polyline=polyline; dd->polygon=polygon; dd->rect=rect; dd->circle=circle;
    dd->path=path; dd->raster=raster; dd->text=text; dd->textUTF8=text_utf8; dd->strWidth=str_width;
    dd->strWidthUTF8=str_width_utf8; dd->metricInfo=metric; dd->clip=clip; dd->size=size; dd->close=close_device;
    dd->hasTextUTF8=TRUE; dd->wantSymbolUTF8=FALSE; dd->canClip=TRUE; dd->deviceClip=TRUE;
    dd->haveRaster=2; dd->haveTransparency=2; dd->haveTransparentBg=2; dd->deviceVersion=R_GE_version;
}
#endif

SEXP rgui2_native_init(SEXP port, SEXP width, SEXP height) {
#ifdef RGUI2_NO_CAIRO
    Rf_error("rgui2 native graphics requires cairo on this platform");
#else
    R_GE_checkVersionOrDie(R_GE_version);
    if (active_device) return R_NilValue;
    Rgui2Device *d=(Rgui2Device*)calloc(1,sizeof(*d));
    d->width=INTEGER(width)[0]; d->height=INTEGER(height)[0];
    d->port_file=strdup(CHAR(STRING_ELT(port,0)));
    d->surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,d->width,d->height);
    d->cr=cairo_create(d->surface);
    pDevDesc dd=GEcreateDD(); memset(dd,0,sizeof(*dd));
    dd->left=0;dd->right=d->width;dd->bottom=0;dd->top=d->height;dd->clipLeft=0;dd->clipRight=d->width;dd->clipBottom=0;dd->clipTop=d->height;
    dd->ipr[0]=dd->ipr[1]=1.0/96.0; dd->cra[0]=dd->cra[1]=12; dd->startps=12; dd->startcol=R_RGB(0,0,0);dd->startfill=R_RGB(255,255,255);dd->startlty=0;dd->startfont=1;dd->startgamma=1;dd->canHAdj=2;dd->deviceSpecific=d;dd->displayListOn=TRUE;
    init_callbacks(dd); d->dd=dd; pGEDevDesc ge=GEcreateDevDesc(dd); GEaddDevice2(ge,"rgui2"); active_device=d;
    return R_NilValue;
#endif
}

SEXP rgui2_native_resize(SEXP width, SEXP height) {
#ifdef RGUI2_NO_CAIRO
    Rf_error("rgui2 native graphics requires cairo on this platform");
#else
    if(!active_device)return R_NilValue;
    int w=INTEGER(width)[0],h=INTEGER(height)[0]; if(w<320)w=320;if(h<240)h=240;
    Rgui2Device*d=active_device; cairo_destroy(d->cr);cairo_surface_destroy(d->surface);d->width=w;d->height=h;
    d->surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);d->cr=cairo_create(d->surface);
    d->dd->right=w; d->dd->top=h; d->dd->clipRight=w; d->dd->clipTop=h;
    return R_NilValue;
#endif
}

SEXP rgui2_native_send(void) {
#ifndef RGUI2_NO_CAIRO
    if (active_device) send_frame(active_device);
#endif
    return R_NilValue;
}

static const R_CallMethodDef callMethods[]={{"rgui2_native_init",(DL_FUNC)&rgui2_native_init,3},{"rgui2_native_resize",(DL_FUNC)&rgui2_native_resize,2},{"rgui2_native_send",(DL_FUNC)&rgui2_native_send,0},{NULL,NULL,0}};
void R_init_rgui2(DllInfo *dll){R_registerRoutines(dll,NULL,callMethods,NULL,NULL);R_useDynamicSymbols(dll,FALSE);}
