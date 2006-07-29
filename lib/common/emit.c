/* $Id$ $Revision$ */
/* vim:set shiftwidth=4 ts=8: */

/**********************************************************
*      This software is part of the graphviz package      *
*                http://www.graphviz.org/                 *
*                                                         *
*            Copyright (c) 1994-2004 AT&T Corp.           *
*                and is licensed under the                *
*            Common Public License, Version 1.0           *
*                      by AT&T Corp.                      *
*                                                         *
*        Information and Software Systems Research        *
*              AT&T Research, Florham Park NJ             *
**********************************************************/

/*
 *  graphics code generator
 */

#include <ctype.h>
#include "render.h"
#include "agxbuf.h"

#define EPSILON .0001

static char *defaultlinestyle[3] = { "solid\0", "setlinewidth\0001\0", 0 };
int    emitState;

/* parse_layers:
 * Split input string into tokens, with separators specified by
 * the layersep attribute. Store the values in the gvc->layerIDs array,
 * starting at index 1, and return the count.
 * Free previously stored list. Note that there is no mechanism
 * to free the memory before exit.
 */
static int parse_layers(GVC_t *gvc, graph_t * g, char *p)
{
    int ntok;
    char *tok;
    int sz;

    gvc->layerDelims = agget(g, "layersep");
    if (!gvc->layerDelims)
        gvc->layerDelims = DEFAULT_LAYERSEP;

    ntok = 0;
    sz = 0;
    gvc->layers = strdup(p);

    for (tok = strtok(gvc->layers, gvc->layerDelims); tok;
         tok = strtok(NULL, gvc->layerDelims)) {
        ntok++;
        if (ntok > sz) {
            sz += SMALLBUF;
            gvc->layerIDs = ALLOC(sz, gvc->layerIDs, char *);
        }
        gvc->layerIDs[ntok] = tok;
    }
    if (ntok) {
        gvc->layerIDs = RALLOC(ntok + 2, gvc->layerIDs, char *);        /* shrink to minimum size */
        gvc->layerIDs[0] = NULL;
        gvc->layerIDs[ntok + 1] = NULL;
    }

    return ntok;
}

/* chkOrder:
 * Determine order of output.
 * Output usually in breadth first graph walk order
 */
static int chkOrder(graph_t * g)
{
    char *p = agget(g, "outputorder");
    if (p) {
        char c = *p;
        if ((c == 'n') && !strcmp(p + 1, "odesfirst"))
            return EMIT_SORTED;
        if ((c == 'e') && !strcmp(p + 1, "dgesfirst"))
            return EMIT_EDGE_SORTED;
    }
    return 0;
}

static void init_job_flags(GVJ_t * job, graph_t * g)
{
    switch (job->output_lang) {
    case GVRENDER_PLUGIN:
        job->flags = chkOrder(g) | job->render.features->flags;
        break;
    case VTX:
        /* output sorted, i.e. all nodes then all edges */
        job->flags = EMIT_SORTED;
        break;
    case DIA:
        /* output in preorder traversal of the graph */
        job->flags = EMIT_PREORDER;
        break;
    case EXTENDED_DOT: case ATTRIBUTED_DOT: case CANONICAL_DOT:
    case PLAIN: case PLAIN_EXT:
        job->flags = 0;
        break;
    default:
        job->flags = chkOrder(g);
        break;
    }
}

static void init_layering(GVC_t * gvc, graph_t * g)
{
    char *str;

    /* free layer strings and pointers from previous graph */
    if (gvc->layers)
	free(gvc->layers);
    if (gvc->layerIDs)
	free(gvc->layerIDs);

    if ((str = agget(g, "layers")) != 0) {
	gvc->numLayers = parse_layers(gvc, g, str);
    } else {
	gvc->layerIDs = NULL;
	gvc->numLayers = 1;
    }
}

static void firstlayer(GVJ_t *job)
{
    job->numLayers = job->gvc->numLayers;
    if ((job->numLayers > 1)
		&& (! (gvrender_features(job) & GVRENDER_DOES_LAYERS))) {
	agerr(AGWARN, "layers not supported in %s output\n",
		job->output_langname);
	job->numLayers = 1;
    }

    job->layerNum = 1;
}

static bool validlayer(GVJ_t *job)
{
    return (job->layerNum <= job->numLayers);
}

static void nextlayer(GVJ_t *job)
{
    job->layerNum++;
}

static point pagecode(GVJ_t *job, char c)
{
    point rv;
    rv.x = rv.y = 0;
    switch (c) {
    case 'T':
	job->pagesArrayFirst.y = job->pagesArraySize.y - 1;
	rv.y = -1;
	break;
    case 'B':
	rv.y = 1;
	break;
    case 'L':
	rv.x = 1;
	break;
    case 'R':
	job->pagesArrayFirst.x = job->pagesArraySize.x - 1;
	rv.x = -1;
	break;
    }
    return rv;
}

static void init_job_pagination(GVJ_t * job, graph_t *g)
{
    GVC_t *gvc = job->gvc;
    pointf pageSize;	/* page size for the graph - points*/
    pointf imageSize;	/* image size on one page of the graph - points */
    pointf margin;	/* margin for a page of the graph - points */

    /* unpaginated image size in device units */
    imageSize = job->view;
    if (job->rotation)
	imageSize = exch_xyf(imageSize);

    /* determine pagination */
    if (gvc->graph_sets_pageSize) {
	/* page was set by user */
	pageSize = gvc->pageSize;

	/* we don't want graph page to exceed its bounding box */
	pageSize.x = MIN(pageSize.x, imageSize.x);
	pageSize.y = MIN(pageSize.y, imageSize.y);

	if (pageSize.x < EPSILON)
	    job->pagesArraySize.x = 1;
	else {
	    job->pagesArraySize.x = (int)(imageSize.x / pageSize.x);
	    if ((imageSize.x - (job->pagesArraySize.x * pageSize.x)) > EPSILON)
		job->pagesArraySize.x++;
	}
	if (pageSize.y < EPSILON)
	    job->pagesArraySize.y = 1;
	else {
	    job->pagesArraySize.y = (int)(imageSize.y / pageSize.y);
	    if ((imageSize.y - (job->pagesArraySize.y * pageSize.y)) > EPSILON)
		job->pagesArraySize.y++;
	}
	job->numPages = job->pagesArraySize.x * job->pagesArraySize.y;

	/* find the drawable size in device coords */
	imageSize.x = MIN(imageSize.x, pageSize.x);
	imageSize.y = MIN(imageSize.y, pageSize.y);
    } else {
	/* page not set by user, assume default when centering,
	   but allow infinite page for any other interpretation */
	pageSize.x = DEFAULT_PAGEWD;
	pageSize.y = DEFAULT_PAGEHT;
	job->pagesArraySize.x = job->pagesArraySize.y = job->numPages = 1;
    }

    /* size of one page in graph units */
    job->pageSize.x = imageSize.x / job->zoom;
    job->pageSize.y = imageSize.y / job->zoom;

    PF2P(job->margin, margin);

    /* determine page box including centering */
    if (GD_drawing(g)->centered) {
	if (pageSize.x > imageSize.x)
	    margin.x += (pageSize.x - imageSize.x) / 2;
	if (pageSize.y > imageSize.y)
	    margin.y += (pageSize.y - imageSize.y) / 2;
    }

    job->canvasBox.LL.x = margin.x;
    job->canvasBox.LL.y = margin.y;
    job->canvasBox.UR.x = margin.x + imageSize.x;
    job->canvasBox.UR.y = margin.y + imageSize.y;

    /* set up pagedir */
    job->pagesArrayMajor.x = job->pagesArrayMajor.y 
		= job->pagesArrayMinor.x = job->pagesArrayMinor.y = 0;
    job->pagesArrayFirst.x = job->pagesArrayFirst.y = 0;
    job->pagesArrayMajor = pagecode(job, gvc->pagedir[0]);
    job->pagesArrayMinor = pagecode(job, gvc->pagedir[1]);
    if ((abs(job->pagesArrayMajor.x + job->pagesArrayMinor.x) != 1)
     || (abs(job->pagesArrayMajor.y + job->pagesArrayMinor.y) != 1)) {
	job->pagesArrayMajor = pagecode(job, 'B');
	job->pagesArrayMinor = pagecode(job, 'L');
	agerr(AGWARN, "pagedir=%s ignored\n", gvc->pagedir);
    }
}

static void firstpage(GVJ_t *job)
{
    job->pagesArrayElem = job->pagesArrayFirst;
}

static bool validpage(GVJ_t *job)
{
    return ((job->pagesArrayElem.x >= 0)
	 && (job->pagesArrayElem.x < job->pagesArraySize.x)
	 && (job->pagesArrayElem.y >= 0)
	 && (job->pagesArrayElem.y < job->pagesArraySize.y));
}

static void nextpage(GVJ_t *job)
{
    job->pagesArrayElem = add_points(job->pagesArrayElem, job->pagesArrayMinor);
    if (validpage(job) == FALSE) {
	if (job->pagesArrayMajor.y)
	    job->pagesArrayElem.x = job->pagesArrayFirst.x;
	else
	    job->pagesArrayElem.y = job->pagesArrayFirst.y;
	job->pagesArrayElem = add_points(job->pagesArrayElem, job->pagesArrayMajor);
    }
}

static bool write_edge_test(Agraph_t * g, Agedge_t * e)
{
    Agraph_t *sg;
    int c;

    for (c = 1; c <= GD_n_cluster(g); c++) {
	sg = GD_clust(g)[c];
	if (agcontains(sg, e))
	    return FALSE;
    }
    return TRUE;
}

static bool write_node_test(Agraph_t * g, Agnode_t * n)
{
    Agraph_t *sg;
    int c;

    for (c = 1; c <= GD_n_cluster(g); c++) {
	sg = GD_clust(g)[c];
	if (agcontains(sg, n))
	    return FALSE;
    }
    return TRUE;
}

void emit_background(GVJ_t * job, graph_t *g)
{
    char *str;

    if (! ((str = agget(g, "bgcolor")) && str[0]))
	str = "white";
    gvrender_set_fillcolor(job, str);
    gvrender_set_pencolor(job, str);
    gvrender_box(job, job->pageBox, TRUE);	/* filled */
}

static void setup_page(GVJ_t * job, graph_t * g)
{
    /* prescaled pad so that its size is constant under scaling */
    pointf pad;

    pad.x = job->pad.x / job->zoom;
    pad.y = job->pad.y / job->zoom;

    /* establish current box in graph coordinates */
    job->pageBox.LL.x = job->pagesArrayElem.x * job->pageSize.x - pad.x;
    job->pageBox.LL.y = job->pagesArrayElem.y * job->pageSize.y - pad.y;
    job->pageBox.UR.x = job->pageBox.LL.x + job->pageSize.x;
    job->pageBox.UR.y = job->pageBox.LL.y + job->pageSize.y;

    /* establish pageOffset to be applied, in graph coordinates */
    if (job->rotation) {
	job->pageOffset.x = -pad.x + job->pageSize.y * (job->pagesArrayElem.y +1);
	job->pageOffset.y =  pad.y - job->pageSize.x * job->pagesArrayElem.x;
    }
    else {
	job->pageOffset.x =  pad.x - job->pageSize.x * job->pagesArrayElem.x;
	job->pageOffset.y =  pad.y - job->pageSize.y * job->pagesArrayElem.y;
    }

    job->pageBoxClip.UR.x = MIN(job->clip.UR.x, job->pageBox.UR.x);
    job->pageBoxClip.UR.y = MIN(job->clip.UR.y, job->pageBox.UR.y);
    job->pageBoxClip.LL.x = MAX(job->clip.LL.x, job->pageBox.LL.x);
    job->pageBoxClip.LL.y = MAX(job->clip.LL.y, job->pageBox.LL.y);

    if (job->rotation) {
        job->pageBoundingBox.LL.x = ROUND(job->canvasBox.LL.y * job->dpi.x / POINTS_PER_INCH);
        job->pageBoundingBox.LL.y = ROUND(job->canvasBox.LL.x * job->dpi.y / POINTS_PER_INCH);
        job->pageBoundingBox.UR.x = ROUND(job->canvasBox.UR.y * job->dpi.x / POINTS_PER_INCH);
        job->pageBoundingBox.UR.y = ROUND(job->canvasBox.UR.x * job->dpi.y / POINTS_PER_INCH);
    }
    else {
        job->pageBoundingBox.LL.x = ROUND(job->canvasBox.LL.x * job->dpi.x / POINTS_PER_INCH);
        job->pageBoundingBox.LL.y = ROUND(job->canvasBox.LL.y * job->dpi.y / POINTS_PER_INCH);
        job->pageBoundingBox.UR.x = ROUND(job->canvasBox.UR.x * job->dpi.x / POINTS_PER_INCH);
        job->pageBoundingBox.UR.y = ROUND(job->canvasBox.UR.y * job->dpi.y / POINTS_PER_INCH);
    }

    if (job->common->viewNum == 0)
        job->boundingBox = job->pageBoundingBox;
    else
        EXPANDBB(job->boundingBox, job->pageBoundingBox);

    if (job->rotation) {
	if (job->flags & GVRENDER_Y_GOES_DOWN) {
	    job->translation.x = -job->pageBox.UR.x - job->pageBoundingBox.LL.x / job->scale.x;
	    job->translation.y = -job->pageBox.UR.y - job->pageBoundingBox.LL.y / job->scale.y;
	}
	else {
	    job->translation.x = -job->pageBox.LL.x + job->pageBoundingBox.LL.y / job->scale.y;
	    job->translation.y = -job->pageBox.UR.y - job->pageBoundingBox.LL.x / job->scale.x;
	}
    }
    else {
	job->translation.x = -job->pageBox.LL.x + job->pageBoundingBox.LL.x / job->scale.x;
	if (job->flags & GVRENDER_Y_GOES_DOWN)
	    job->translation.y = -job->pageBox.UR.y - job->pageBoundingBox.LL.y / job->scale.y;
	else
	    job->translation.y = -job->pageBox.LL.y + job->pageBoundingBox.LL.y / job->scale.y;
    }

    job->compscale = job->scale;
    job->compscale.y *= (job->flags & GVRENDER_Y_GOES_DOWN) ? -1. : 1.;
}

#if 0
static bool node_in_view(GVJ_t *job, node_t * n)
{
    boxf b;

    if (boxf_contains(job->clip, job->pageBox) && job->numPages == 1)
       return TRUE;
    b.LL.x = ND_coord_i(n).x - ND_lw_i(n);
    b.LL.y = ND_coord_i(n).y - ND_ht_i(n) / 2.;
    b.UR.x = ND_coord_i(n).x + ND_rw_i(n);
    b.UR.y = ND_coord_i(n).y + ND_ht_i(n) / 2.;

    return boxf_overlap(job->pageBoxClip, b);
}
#endif

static bool is_natural_number(char *sstr)
{
    unsigned char *str = (unsigned char *) sstr;

    while (*str)
	if (NOT(isdigit(*str++)))
	    return FALSE;
    return TRUE;
}

static int layer_index(GVC_t *gvc, char *str, int all)
{
    GVJ_t *job = gvc->job;
    int i;

    if (streq(str, "all"))
	return all;
    if (is_natural_number(str))
	return atoi(str);
    if (gvc->layerIDs)
	for (i = 1; i <= job->numLayers; i++)
	    if (streq(str, gvc->layerIDs[i]))
		return i;
    return -1;
}

static bool selectedlayer(GVJ_t *job, char *spec)
{
    GVC_t *gvc = job->gvc;
    int n0, n1;
    unsigned char buf[SMALLBUF];
    char *w0, *w1;
    agxbuf xb;
    bool rval = FALSE;

    agxbinit(&xb, SMALLBUF, buf);
    agxbput(&xb, spec);
    w1 = w0 = strtok(agxbuse(&xb), gvc->layerDelims);
    if (w0)
	w1 = strtok(NULL, gvc->layerDelims);
    switch ((w0 != NULL) + (w1 != NULL)) {
    case 0:
	rval = FALSE;
	break;
    case 1:
	n0 = layer_index(gvc, w0, job->layerNum);
	rval = (n0 == job->layerNum);
	break;
    case 2:
	n0 = layer_index(gvc, w0, 0);
	n1 = layer_index(gvc, w1, job->numLayers);
	if ((n0 < 0) || (n1 < 0))
	    rval = TRUE;
	else if (n0 > n1) {
	    int t = n0;
	    n0 = n1;
	    n1 = t;
	}
	rval = BETWEEN(n0, job->layerNum, n1);
	break;
    }
    agxbfree(&xb);
    return rval;
}

static bool node_in_layer(GVJ_t *job, graph_t * g, node_t * n)
{
    char *pn, *pe;
    edge_t *e;

    if (job->numLayers <= 1)
	return TRUE;
    pn = late_string(n, N_layer, "");
    if (selectedlayer(job, pn))
	return TRUE;
    if (pn[0])
	return FALSE;		/* Only check edges if pn = "" */
    if ((e = agfstedge(g, n)) == NULL)
	return TRUE;
    for (e = agfstedge(g, n); e; e = agnxtedge(g, e, n)) {
	pe = late_string(e, E_layer, "");
	if ((pe[0] == '\0') || selectedlayer(job, pe))
	    return TRUE;
    }
    return FALSE;
}

static bool edge_in_layer(GVJ_t *job, graph_t * g, edge_t * e)
{
    char *pe, *pn;
    int cnt;

    if (job->numLayers <= 1)
	return TRUE;
    pe = late_string(e, E_layer, "");
    if (selectedlayer(job, pe))
	return TRUE;
    if (pe[0])
	return FALSE;
    for (cnt = 0; cnt < 2; cnt++) {
	pn = late_string(cnt < 1 ? e->tail : e->head, N_layer, "");
	if ((pn[0] == '\0') || selectedlayer(job, pn))
	    return TRUE;
    }
    return FALSE;
}

static bool clust_in_layer(GVJ_t *job, graph_t * sg)
{
    char *pg;
    node_t *n;

    if (job->numLayers <= 1)
	return TRUE;
    pg = late_string(sg, agfindattr(sg, "layer"), "");
    if (selectedlayer(job, pg))
	return TRUE;
    if (pg[0])
	return FALSE;
    for (n = agfstnode(sg); n; n = agnxtnode(sg, n))
	if (node_in_layer(job, sg, n))
	    return TRUE;
    return FALSE;
}

#if 1
static bool node_in_box(node_t *n, boxf b)
{
    return boxf_overlap(ND_bb(n), b);
}
#endif

static void emit_node(GVJ_t * job, node_t * n)
{
    GVC_t *gvc = job->gvc;
    char *s;
    int oldstate;

    if (ND_shape(n) == NULL)
	return;

    oldstate = gvc->emit_state;
    gvc->emit_state = EMIT_NDRAW;
    if (node_in_layer(job, n->graph, n)
	    && node_in_box(n, job->pageBoxClip)
	    && (ND_state(n) != gvc->common.viewNum)) {

        gvrender_comment(job, n->name);

	s = late_string(n, N_comment, "");
	if (s[0])
	    gvrender_comment(job, s);
        
	gvrender_begin_node(job, n);
	setColorScheme (agget (n, "colorscheme"));
	gvrender_begin_context(job);
	ND_shape(n)->fns->codefn(job, n);
	ND_state(n) = gvc->common.viewNum;
	gvrender_end_context(job);
	gvrender_end_node(job);
    }
    gvc->emit_state = oldstate;
}

/* calculate an offset vector, length d, perpendicular to line p,q */
static pointf computeoffset_p(pointf p, pointf q, double d)
{
    pointf res;
    double x = p.x - q.x, y = p.y - q.y;

    /* keep d finite as line length approaches 0 */
    d /= sqrt(x * x + y * y + EPSILON);
    res.x = y * d;
    res.y = -x * d;
    return res;
}

/* calculate offset vector, length d, perpendicular to spline p,q,r,s at q&r */
static pointf computeoffset_qr(pointf p, pointf q, pointf r, pointf s,
			       double d)
{
    pointf res;
    double len;
    double x = q.x - r.x, y = q.y - r.y;

    len = sqrt(x * x + y * y);
    if (len < EPSILON) {
	/* control points are on top of each other
	   use slope between endpoints instead */
	x = p.x - s.x, y = p.y - s.y;
	/* keep d finite as line length approaches 0 */
	len = sqrt(x * x + y * y + EPSILON);
    }
    d /= len;
    res.x = y * d;
    res.y = -x * d;
    return res;
}

static void emit_attachment(GVJ_t * job, textlabel_t * lp, splines * spl)
{
    pointf sz, AF[3];
    point p;
    unsigned char *s;

    for (s = (unsigned char *) (lp->text); *s; s++) {
	if (isspace(*s) == FALSE)
	    break;
    }
    if (*s == 0)
	return;

    sz = lp->dimen;
    AF[0] = pointfof((double)(lp->p.x) + sz.x / 2., (double)(lp->p.y) - sz.y / 2.);
    AF[1] = pointfof(AF[0].x - sz.x, AF[0].y);
    p = dotneato_closest(spl, lp->p);
    P2PF(p,AF[2]);
    /* Don't use edge style to draw attachment */
    gvrender_set_style(job, job->gvc->defaultlinestyle);
    /* Use font color to draw attachment
       - need something unambiguous in case of multicolored parallel edges
       - defaults to black for html-like labels
     */
    gvrender_set_pencolor(job, lp->fontcolor);
    gvrender_polyline(job, AF, 3);
}

#if 0
static bool edge_in_view(GVJ_t *job, edge_t * e)
{
    int i, j, np;
    bezier bz;
    point *p;
    pointf pp, pn;
    double sx, sy;
    boxf b;
    textlabel_t *lp;

    if (boxf_contains(job->clip, job->pageBox) && job->numPages == 1)
       return TRUE;
    if (ED_spl(e) == NULL)
       return FALSE;
    for (i = 0; i < ED_spl(e)->size; i++) {
       bz = ED_spl(e)->list[i];
       np = bz.size;
       p = bz.list;
       P2PF(p[0],pp);
       for (j = 0; j < np; j++) {
           P2PF(p[j],pn);
           b = mkboxf(pp, pn);
           if (boxf_overlap(job->pageBoxClip, b))
               return TRUE;
           pp = pn;
       }
    }
    if ((lp = ED_label(e)) == NULL)
       return FALSE;
    sx = lp->dimen.x / 2.;
    sy = lp->dimen.y / 2.;
    b.LL.x = lp->p.x - sx;
    b.UR.x = lp->p.x + sx;
    b.LL.y = lp->p.y - sy;
    b.UR.y = lp->p.y + sy;
    return boxf_overlap(job->pageBoxClip, b);
}
#endif

/* edges colors can be mutiple colors separated by ":"
 * so we commpute a default pencolor with the same number of colors. */
static char* default_pencolor(char *pencolor, char *deflt)
{
    static char *buf;
    static int bufsz;
    char *p;
    int len, ncol;

    ncol = 1;
    for (p = pencolor; *p; p++) {
	if (*p == ':')
	    ncol++;
    }
    len = ncol * (strlen(deflt) + 1);
    if (bufsz < len) {
	bufsz = len + 10;
	buf = realloc(buf, bufsz);
    }
    strcpy(buf, deflt);
    while(--ncol) {
	strcat(buf, ":");
	strcat(buf, deflt);
    }
    return buf;
}

void emit_edge_graphics(GVJ_t * job, edge_t * e)
{
    int i, j, cnum, numc = 0;
    char *color, *pencolor, *fillcolor, *style;
    char *headcolor, *tailcolor, *lastcolor;
    char *colors = NULL;
    char **styles = 0;
    char **sp;
    bezier bz = { 0, 0, 0, 0 };
    bezierf bzf;
    splinesf offspl, tmpspl;
    pointf pf0, pf1, pf2 = { 0, 0 }, pf3, *offlist, *tmplist;
    bool saved = FALSE;
    double scale, numc2;
    char *p;
    int oldstate;

#define SEP 2.0

    oldstate = job->gvc->emit_state;
    job->gvc->emit_state = EMIT_EDRAW;
    style = late_string(e, E_style, "");
    /* We shortcircuit drawing an invisible edge because the arrowhead
     * code resets the style to solid, and most of the code generators
     * (except PostScript) won't honor a previous style of invis.
     */
    if (style[0]) {
	styles = parse_style(style);
	sp = styles;
	while ((p = *sp++)) {
	    if (streq(p, "invis"))
		return;
	}
    }
    setColorScheme (agget (e, "colorscheme"));
    if (ED_spl(e)) {
	scale = late_double(e, E_arrowsz, 1.0, 0.0);
	color = late_string(e, E_color, "");

	if (color[0] || styles) {
	    gvrender_begin_context(job);
	    if (styles)
		gvrender_set_style(job, styles);
	    saved = TRUE;
	}
	/* need to know how many colors separated by ':' */
	for (p = color; *p; p++)
	    if (*p == ':')
		numc++;

	fillcolor = pencolor = color;
	if (ED_gui_state(e) & GUI_STATE_ACTIVE) {
	    pencolor = late_nnstring(e, E_activepencolor,
			default_pencolor(pencolor, DEFAULT_ACTIVEPENCOLOR));
	    fillcolor = late_nnstring(e, E_activefillcolor, DEFAULT_ACTIVEFILLCOLOR);
	}
	else if (ED_gui_state(e) & GUI_STATE_SELECTED) {
	    pencolor = late_nnstring(e, E_selectedpencolor,
			default_pencolor(pencolor, DEFAULT_SELECTEDPENCOLOR));
	    fillcolor = late_nnstring(e, E_selectedfillcolor, DEFAULT_SELECTEDFILLCOLOR);
	}
	else if (ED_gui_state(e) & GUI_STATE_DELETED) {
	    pencolor = late_nnstring(e, E_deletedpencolor,
			default_pencolor(pencolor, DEFAULT_DELETEDPENCOLOR));
	    fillcolor = late_nnstring(e, E_deletedfillcolor, DEFAULT_DELETEDFILLCOLOR);
	}
	else if (ED_gui_state(e) & GUI_STATE_VISITED) {
	    pencolor = late_nnstring(e, E_visitedpencolor,
			default_pencolor(pencolor, DEFAULT_VISITEDPENCOLOR));
	    fillcolor = late_nnstring(e, E_visitedfillcolor, DEFAULT_VISITEDFILLCOLOR);
	}
	if (pencolor != color)
    	    gvrender_set_pencolor(job, pencolor);
	if (fillcolor != color)
	    gvrender_set_fillcolor(job, fillcolor);
	color = pencolor;
	/* if more than one color - then generate parallel beziers, one per color */
	if (numc) {
	    /* calculate and save offset vector spline and initialize first offset spline */
	    tmpspl.size = offspl.size = ED_spl(e)->size;
	    offspl.list = malloc(sizeof(bezier) * offspl.size);
	    tmpspl.list = malloc(sizeof(bezier) * tmpspl.size);
	    numc2 = (2 + numc) / 2.0;
	    for (i = 0; i < offspl.size; i++) {
		bz = ED_spl(e)->list[i];
		tmpspl.list[i].size = offspl.list[i].size = bz.size;
		offlist = offspl.list[i].list =
		    malloc(sizeof(pointf) * bz.size);
		tmplist = tmpspl.list[i].list =
		    malloc(sizeof(pointf) * bz.size);
		P2PF(bz.list[0], pf3);
		for (j = 0; j < bz.size - 1; j += 3) {
		    pf0 = pf3;
		    P2PF(bz.list[j + 1], pf1);
		    /* calculate perpendicular vectors for each bezier point */
		    if (j == 0)	/* first segment, no previous pf2 */
			offlist[j] = computeoffset_p(pf0, pf1, SEP);
		    else	/* i.e. pf2 is available from previous segment */
			offlist[j] = computeoffset_p(pf2, pf1, SEP);

		    P2PF(bz.list[j + 2], pf2);
		    P2PF(bz.list[j + 3], pf3);
		    offlist[j + 1] = offlist[j + 2] =
			computeoffset_qr(pf0, pf1, pf2, pf3, SEP);
		    /* initialize tmpspl to outermost position */
		    tmplist[j].x = pf0.x - numc2 * offlist[j].x;
		    tmplist[j].y = pf0.y - numc2 * offlist[j].y;
		    tmplist[j + 1].x = pf1.x - numc2 * offlist[j + 1].x;
		    tmplist[j + 1].y = pf1.y - numc2 * offlist[j + 1].y;
		    tmplist[j + 2].x = pf2.x - numc2 * offlist[j + 2].x;
		    tmplist[j + 2].y = pf2.y - numc2 * offlist[j + 2].y;
		}
		/* last segment, no next pf1 */
		offlist[j] = computeoffset_p(pf2, pf3, SEP);
		tmplist[j].x = pf3.x - numc2 * offlist[j].x;
		tmplist[j].y = pf3.y - numc2 * offlist[j].y;
	    }
	    lastcolor = headcolor = tailcolor = color;
	    colors = strdup(color);
	    for (cnum = 0, color = strtok(colors, ":"); color;
		cnum++, color = strtok(0, ":")) {
		if (!color[0])
		    color = DEFAULT_COLOR;
		if (color != lastcolor) {
	            if (! (ED_gui_state(e) & (GUI_STATE_ACTIVE | GUI_STATE_SELECTED))) {
		        gvrender_set_pencolor(job, color);
		        gvrender_set_fillcolor(job, color);
		    }
		    lastcolor = color;
		}
		if (cnum == 0)
		    headcolor = tailcolor = color;
		if (cnum == 1)
		    tailcolor = color;
		for (i = 0; i < tmpspl.size; i++) {
		    tmplist = tmpspl.list[i].list;
		    offlist = offspl.list[i].list;
		    for (j = 0; j < tmpspl.list[i].size; j++) {
			tmplist[j].x += offlist[j].x;
			tmplist[j].y += offlist[j].y;
		    }
		    gvrender_beziercurve(job, tmplist, tmpspl.list[i].size,
					 FALSE, FALSE, FALSE);
		}
	    }
	    if (bz.sflag) {
		if (color != tailcolor) {
		    color = tailcolor;
	            if (! (ED_gui_state(e) & (GUI_STATE_ACTIVE | GUI_STATE_SELECTED))) {
		        gvrender_set_pencolor(job, color);
		        gvrender_set_fillcolor(job, color);
		    }
		}
		arrow_gen(job, EMIT_TDRAW, bz.sp, bz.list[0],
			scale, bz.sflag);
	    }
	    if (bz.eflag) {
		if (color != headcolor) {
		    color = headcolor;
	            if (! (ED_gui_state(e) & (GUI_STATE_ACTIVE | GUI_STATE_SELECTED))) {
		        gvrender_set_pencolor(job, color);
		        gvrender_set_fillcolor(job, color);
		    }
		}
		arrow_gen(job, EMIT_HDRAW, bz.ep, bz.list[bz.size - 1],
			scale, bz.eflag);
	    }
	    free(colors);
	    for (i = 0; i < offspl.size; i++) {
		free(offspl.list[i].list);
		free(tmpspl.list[i].list);
	    }
	    free(offspl.list);
	    free(tmpspl.list);
	} else {
	    if (! (ED_gui_state(e) & (GUI_STATE_ACTIVE | GUI_STATE_SELECTED))) {
	        if (color[0]) {
		    gvrender_set_pencolor(job, color);
		    gvrender_set_fillcolor(job, color);
	        } else {
		    gvrender_set_pencolor(job, DEFAULT_COLOR);
		    gvrender_set_fillcolor(job, DEFAULT_COLOR);
	        }
	    }
	    for (i = 0; i < ED_spl(e)->size; i++) {
		bz = ED_spl(e)->list[i];
		/* convert points to pointf for gvrender api */
		bzf.size = bz.size;
		bzf.list = malloc(sizeof(pointf) * bzf.size);
		for (j = 0; j < bz.size; j++)
		    P2PF(bz.list[j], bzf.list[j]);
		if (gvrender_features(job) & GVRENDER_DOES_ARROWS) {
		    gvrender_beziercurve(job, bzf.list, bz.size, bz.sflag,
					 bz.eflag, FALSE);
		} else {
		    gvrender_beziercurve(job, bzf.list, bz.size, FALSE,
					 FALSE, FALSE);
		    if (bz.sflag) {
			arrow_gen(job, EMIT_TDRAW, bz.sp, bz.list[0],
				scale, bz.sflag);
		    }
		    if (bz.eflag) {
			arrow_gen(job, EMIT_HDRAW, bz.ep, bz.list[bz.size - 1],
				scale, bz.eflag);
		    }
		}
		free(bzf.list);
	    }
	}
    }
    if (ED_label(e)) {
	emit_label(job, EMIT_ELABEL, ED_label(e), (void *) e);
	if (mapbool(late_string(e, E_decorate, "false")) && ED_spl(e))
	    emit_attachment(job, ED_label(e), ED_spl(e));
    }
    if (ED_head_label(e))
	emit_label(job, EMIT_HLABEL, ED_head_label(e), (void *) e);	/* vladimir */
    if (ED_tail_label(e))
	emit_label(job, EMIT_TLABEL, ED_tail_label(e), (void *) e);	/* vladimir */

    if (saved)
	gvrender_end_context(job);
    job->gvc->emit_state = oldstate;
}

static bool edge_in_box(edge_t *e, boxf b)
{
    splines *spl;
    textlabel_t *lp;

    spl = ED_spl(e);
    if (spl && boxf_overlap(spl->bb, b))
        return TRUE;

    lp = ED_label(e);
    if (lp && overlap_label(lp, b))
        return TRUE;

    return FALSE;
}

static void emit_edge(GVJ_t * job, edge_t * e)
{
    char *s;
    int oldstate;

    if (! edge_in_box(e, job->pageBoxClip) || ! edge_in_layer(job, e->head->graph, e))
	return;

    oldstate = job->gvc->emit_state;
    job->gvc->emit_state = EMIT_EDRAW;
    s = malloc(strlen(e->tail->name) + 2 + strlen(e->head->name) + 1);
    strcpy(s,e->tail->name);
    if (AG_IS_DIRECTED(e->tail->graph))
        strcat(s,"->");
    else
        strcat(s,"--");
    strcat(s,e->head->name);
    gvrender_comment(job, s);
    free(s);

    s = late_string(e, E_comment, "");
    if (s[0])
        gvrender_comment(job, s);

    gvrender_begin_edge(job, e);
    emit_edge_graphics (job, e);
    gvrender_end_edge(job);
    job->gvc->emit_state = oldstate;
}

static void init_gvc(GVC_t * gvc, graph_t * g)
{
    double xf, yf;
    char *p;
    int i;

    gvc->g = g;

    /* margins */
    gvc->graph_sets_margin = FALSE;
    if ((p = agget(g, "margin"))) {
        i = sscanf(p, "%lf,%lf", &xf, &yf);
        if (i > 0) {
            gvc->margin.x = gvc->margin.y = xf * POINTS_PER_INCH;
            if (i > 1)
                gvc->margin.y = yf * POINTS_PER_INCH;
            gvc->graph_sets_margin = TRUE;
        }
    }

    /* pagesize */
    gvc->graph_sets_pageSize = FALSE;
    P2PF(GD_drawing(g)->page, gvc->pageSize);
    if ((GD_drawing(g)->page.x > 0) && (GD_drawing(g)->page.y > 0)) {
        gvc->graph_sets_pageSize = TRUE;
    }

    /* rotation */
    if (GD_drawing(g)->landscape) {
	gvc->rotation = 90;
	/* we expect the user to have swapped x,y coords of pagesize and margin */
	gvc->pageSize = exch_xyf(gvc->pageSize);
	gvc->margin = exch_xyf(gvc->margin);
    }
    else {
	gvc->rotation = 0;
    }

    /* pagedir */
    gvc->pagedir = "BL";
    if ((p = agget(g, "pagedir")) && p[0])
            gvc->pagedir = p;

    /* bounding box */
    B2BF(GD_bb(g),gvc->bb);

    /* clusters have peripheries */
    G_peripheries = agfindattr(g, "peripheries");

    /* default font */
    gvc->defaultfontname = late_nnstring(g->proto->n,
                N_fontname, DEFAULT_FONTNAME);
    gvc->defaultfontsize = late_double(g->proto->n,
                N_fontsize, DEFAULT_FONTSIZE, MIN_FONTSIZE);

    /* default line style */
    gvc->defaultlinestyle = defaultlinestyle;

    gvc->graphname = g->name;
}

static void init_job_margin(GVJ_t *job)
{
    GVC_t *gvc = job->gvc;
    
    if (gvc->graph_sets_margin) {
	job->margin = gvc->margin;
    }
    else {
        /* set default margins depending on format */
        switch (job->output_lang) {
        case GVRENDER_PLUGIN:
            job->margin.x = job->margin.y = job->render.features->default_margin;
            break;
        case HPGL: case PCL: case MIF: case METAPOST: case VTX: case ATTRIBUTED_DOT:
        case PLAIN: case PLAIN_EXT: case QPDF:
            job->margin.x = job->margin.y = DEFAULT_PRINT_MARGIN;
            break;
        case CANONICAL_DOT:
            job->margin.x = job->margin.y = 0;
            break;
        default:
            job->margin.x = job->margin.y = DEFAULT_EMBED_MARGIN;
            break;
        }
    }
}

static void init_job_dpi(GVJ_t *job, graph_t *g)
{
    if (GD_drawing(g)->dpi != 0) {
        job->dpi.x = job->dpi.y = (double)(GD_drawing(g)->dpi);
    }
    else {
        /* set default margins depending on format */
        switch (job->output_lang) {
        case GVRENDER_PLUGIN:
            job->dpi = job->render.features->default_dpi;
            break;
        default:
            job->dpi.x = job->dpi.y = (double)(DEFAULT_DPI);
            break;
        }
    }
}

static void init_job_viewport(GVJ_t * job, graph_t * g)
{
    pointf UR, size;
    char *str;
    double X, Y, Z, x, y;
    int rv;

    assert((GD_bb(g).LL.x == 0) && (GD_bb(g).LL.y == 0));
    P2PF(GD_bb(g).UR, UR);

    /* may want to take this from an attribute someday */
    job->pad.x = job->pad.y = DEFAULT_GRAPH_PAD;

    /* determine final drawing size and scale to apply. */
    /* N.B. size given by user is not rotated by landscape mode */
    /* start with "natural" size of layout */

    Z = 1.0;
    if (GD_drawing(g)->size.x > 0) {	/* graph size was given by user... */
	P2PF(GD_drawing(g)->size, size);
	size.x -= (2 * job->pad.x);
	size.y -= (2 * job->pad.y);
	if ((size.x < UR.x) || (size.y < UR.y) /* drawing is too big... */
	    || ((GD_drawing(g)->filled) /* or ratio=filled requested and ... */
		&& (size.x > UR.x) && (size.y > UR.y))) /* drawing is too small... */
	    Z = MIN(size.x/UR.x, size.y/UR.y);
    }
    
    /* default focus, in graph units = center of bb */
    x = UR.x / 2.;
    y = UR.y / 2.;

    /* rotate and scale bb to give default device width and height */
    if (GD_drawing(g)->landscape)
	UR = exch_xyf(UR);

    /* calculate default viewport size in points */
    X = Z * UR.x + 2 * job->pad.x;
    Y = Z * UR.y + 2 * job->pad.y;

    /* user can override */
    if ((str = agget(g, "viewport")))
	rv = sscanf(str, "%lf,%lf,%lf,%lf,%lf", &X, &Y, &Z, &x, &y);
    /* rv is ignored since args retain previous values if not scanned */

    job->view.x = X; 		/* viewport - points */
    job->view.y = Y;
    job->zoom = Z;              /* scaling factor */
    job->focus.x = x;           /* focus - graph units */
    job->focus.y = y;
    job->rotation = job->gvc->rotation;
}

#define EPSILON .0001

/* setup_view is called for a particular device and refresh event.
 * actual dpi is now known.
 * width and height might have changed through window resizing
 */
static void setup_view(GVJ_t * job, graph_t * g)
{
    double sx, sy; /* half width, half height in graph-units */

    /* compute width,height in device units */
    /* FIXME - width/height also calculated in xlib finalize() using the same formula
     * to get initial windows size.  Should be done in one place only. */
    job->width = ROUND((job->view.x + 2 * job->margin.x) * job->dpi.x / POINTS_PER_INCH);
    job->height = ROUND((job->view.y + 2 * job->margin.y) * job->dpi.y / POINTS_PER_INCH);

    job->scale.x = job->zoom * job->dpi.x / POINTS_PER_INCH;
    job->scale.y = job->zoom * job->dpi.y / POINTS_PER_INCH;

    sx = job->width / (job->scale.x * 2.);
    sy = job->height / (job->scale.y * 2.);

    /* calculate clip region in graph units */
    if (job->rotation) {
        job->clip.UR.x = job->focus.x + sy + EPSILON;
        job->clip.UR.y = job->focus.y + sx + EPSILON;
        job->clip.LL.x = job->focus.x - sy - EPSILON;
        job->clip.LL.y = job->focus.y - sx - EPSILON;
    } else {
        job->clip.UR.x = job->focus.x + sx + EPSILON;
        job->clip.UR.y = job->focus.y + sy + EPSILON;
        job->clip.LL.x = job->focus.x - sx - EPSILON;
        job->clip.LL.y = job->focus.y - sy - EPSILON;
    }
}

static void emit_colors(GVJ_t * job, graph_t * g)
{
    graph_t *sg;
    node_t *n;
    edge_t *e;
    int c;
    char *str, *colors;

    gvrender_set_fillcolor(job, DEFAULT_FILL);
    if (((str = agget(g, "bgcolor")) != 0) && str[0])
	gvrender_set_fillcolor(job, str);
    if (((str = agget(g, "fontcolor")) != 0) && str[0])
	gvrender_set_pencolor(job, str);
    for (c = 1; c <= GD_n_cluster(g); c++) {
	sg = GD_clust(g)[c];
	if (((str = agget(sg, "color")) != 0) && str[0])
	    gvrender_set_pencolor(job, str);
	if (((str = agget(sg, "fillcolor")) != 0) && str[0])
	    gvrender_set_fillcolor(job, str);
	if (((str = agget(sg, "fontcolor")) != 0) && str[0])
	    gvrender_set_pencolor(job, str);
    }
    for (n = agfstnode(g); n; n = agnxtnode(g, n)) {
	if (((str = agget(n, "color")) != 0) && str[0])
	    gvrender_set_pencolor(job, str);
	if (((str = agget(n, "fillcolor")) != 0) && str[0])
	    gvrender_set_fillcolor(job, str);
	if (((str = agget(n, "fontcolor")) != 0) && str[0])
	    gvrender_set_pencolor(job, str);
	for (e = agfstout(g, n); e; e = agnxtout(g, e)) {
	    if (((str = agget(e, "color")) != 0) && str[0]) {
		if (strchr(str, ':')) {
		    colors = strdup(str);
		    for (str = strtok(colors, ":"); str;
			str = strtok(0, ":")) {
			if (str[0])
			    gvrender_set_pencolor(job, str);
		    }
		    free(colors);
		} else
		    gvrender_set_pencolor(job, str);
	    }
	    if (((str = agget(e, "fontcolor")) != 0) && str[0])
		gvrender_set_pencolor(job, str);
	}
    }
}

void emit_view(GVJ_t * job, graph_t * g, int flags)
{
    GVC_t * gvc = job->gvc;
    node_t *n;
    edge_t *e;

    gvc->common.viewNum++;
    if (GD_label(g))
	emit_label(job, EMIT_GLABEL, GD_label(g), (void *) g);
    /* when drawing, lay clusters down before nodes and edges */
    if (!(flags & EMIT_CLUSTERS_LAST))
	emit_clusters(job, g, flags);
    if (flags & EMIT_SORTED) {
	/* output all nodes, then all edges */
	gvrender_begin_nodes(job);
	for (n = agfstnode(g); n; n = agnxtnode(g, n))
	    emit_node(job, n);
	gvrender_end_nodes(job);
	gvrender_begin_edges(job);
	for (n = agfstnode(g); n; n = agnxtnode(g, n)) {
	    for (e = agfstout(g, n); e; e = agnxtout(g, e))
		emit_edge(job, e);
	}
	gvrender_end_edges(job);
    } else if (flags & EMIT_EDGE_SORTED) {
	/* output all edges, then all nodes */
	gvrender_begin_edges(job);
	for (n = agfstnode(g); n; n = agnxtnode(g, n))
	    for (e = agfstout(g, n); e; e = agnxtout(g, e))
		emit_edge(job, e);
	gvrender_end_edges(job);
	gvrender_begin_nodes(job);
	for (n = agfstnode(g); n; n = agnxtnode(g, n))
	    emit_node(job, n);
	gvrender_end_nodes(job);
    } else if (flags & EMIT_PREORDER) {
	gvrender_begin_nodes(job);
	for (n = agfstnode(g); n; n = agnxtnode(g, n))
	    if (write_node_test(g, n))
		emit_node(job, n);
	gvrender_end_nodes(job);
	gvrender_begin_edges(job);

	for (n = agfstnode(g); n; n = agnxtnode(g, n)) {
	    for (e = agfstout(g, n); e; e = agnxtout(g, e)) {
		if (write_edge_test(g, e))
		    emit_edge(job, e);
	    }
	}
	gvrender_end_edges(job);
    } else {
	/* output in breadth first graph walk order */
	for (n = agfstnode(g); n; n = agnxtnode(g, n)) {
	    emit_node(job, n);
	    for (e = agfstout(g, n); e; e = agnxtout(g, e)) {
		emit_node(job, e->head);
		emit_edge(job, e);
	    }
	}
    }
    /* when mapping, detect events on clusters after nodes and edges */
    if (flags & EMIT_CLUSTERS_LAST)
	emit_clusters(job, g, flags);
    gvrender_end_page(job);
}

void emit_graph(GVJ_t * job, graph_t * g)
{
    node_t *n;
    char *s;
    int flags = job->flags;
    GVC_t *gvc = job->gvc;

    setup_view(job, g);

    s = late_string(g, agfindattr(g, "comment"), "");
    gvrender_comment(job, s);

    gvrender_begin_graph(job, g);
    if (flags & EMIT_COLORS)
	emit_colors(job,g);

    for (n = agfstnode(g); n; n = agnxtnode(g, n))
	ND_state(n) = 0;
    /* iterate layers */
    for (firstlayer(job); validlayer(job); nextlayer(job)) {
	if (job->numLayers > 1)
	    gvrender_begin_layer(job);

	/* iterate pages */
	for (firstpage(job); validpage(job); nextpage(job)) {
	    setColorScheme (agget (g, "colorscheme"));
    	    setup_page(job, g);
	    gvrender_begin_page(job);
	    gvrender_set_pencolor(job, DEFAULT_COLOR);
	    gvrender_set_fillcolor(job, DEFAULT_FILL);
	    gvrender_set_font(job, gvc->defaultfontname, gvc->defaultfontsize);
	    if (job->numLayers == 1)
		emit_background(job, g);
	    if (boxf_overlap(job->clip, job->pageBox))
	        emit_view(job,g,flags);
	} 

	if (job->numLayers > 1)
	    gvrender_end_layer(job);
    } 
    gvrender_end_graph(job);
}

/* support for stderr_once */
static void free_string_entry(Dict_t * dict, char *key, Dtdisc_t * disc)
{
    agstrfree(key);
}

static Dict_t *strings;
static Dtdisc_t stringdict = {
    0,				/* key  - the object itself */
    0,				/* size - null-terminated string */
    -1,				/* link - allocate separate holder objects  */
    NIL(Dtmake_f),
    (Dtfree_f) free_string_entry,
    NIL(Dtcompar_f),
    NIL(Dthash_f),
    NIL(Dtmemory_f),
    NIL(Dtevent_f)
};

int emit_once(char *str)
{
    if (strings == 0)
	strings = dtopen(&stringdict, Dtoset);
    if (!dtsearch(strings, str)) {
	dtinsert(strings, agstrdup(str));
	return TRUE;
    }
    return FALSE;
}

void emit_once_reset(void)
{
    if (strings) {
	dtclose(strings);
	strings = 0;
    }
}

static char **checkClusterStyle(graph_t* sg, int *flagp)
{
    char *style;
    char **pstyle = 0;
    int istyle = 0;

    if (((style = agget(sg, "style")) != 0) && style[0]) {
	char **pp;
	char **qp;
	char *p;
	pp = pstyle = parse_style(style);
	while ((p = *pp)) {
	    if (strcmp(p, "filled") == 0) {
		istyle |= FILLED;
		pp++;
	    } else if (strcmp(p, "rounded") == 0) {
		istyle |= ROUNDED;
		qp = pp; /* remove rounded from list passed to renderer */
		do {
		    qp++;
		    *(qp-1) = *qp;
		} while (*qp);
	    } else pp++;
	}
    }

    *flagp = istyle;
    return pstyle;
}

void emit_clusters(GVJ_t * job, Agraph_t * g, int flags)
{
    int c, istyle, filled;
    graph_t *sg;
    boxf BF;
    pointf AF[4];
    char *color, *fillcolor, *pencolor, **style;
    node_t *n;
    edge_t *e;
    int oldstate;

    oldstate = job->gvc->emit_state;
    job->gvc->emit_state = EMIT_CDRAW;
    for (c = 1; c <= GD_n_cluster(g); c++) {
	sg = GD_clust(g)[c];
	if (clust_in_layer(job, sg) == FALSE)
	    continue;
	/* when mapping, detect events on clusters after sub_clusters */
	if (flags & EMIT_CLUSTERS_LAST)
	    emit_clusters(job, sg, flags);
	gvrender_begin_cluster(job, sg);
	setColorScheme (agget (sg, "colorscheme"));
	gvrender_begin_context(job);
	filled = FALSE;
	istyle = 0;
	if ((style = checkClusterStyle(sg, &istyle))) {
	    gvrender_set_style(job, style);
	    if (istyle & FILLED)
		filled = TRUE;
	}
	fillcolor = pencolor = 0;
	if (GD_gui_state(sg) & GUI_STATE_ACTIVE) {
	    pencolor = late_nnstring(sg, G_activepencolor, DEFAULT_ACTIVEPENCOLOR);
	    fillcolor = late_nnstring(sg, G_activefillcolor, DEFAULT_ACTIVEFILLCOLOR);
	    filled = TRUE;
	}
	else if (GD_gui_state(sg) & GUI_STATE_SELECTED) {
	    pencolor = late_nnstring(sg, G_activepencolor, DEFAULT_SELECTEDPENCOLOR);
	    fillcolor = late_nnstring(sg, G_activefillcolor, DEFAULT_SELECTEDFILLCOLOR);
	    filled = TRUE;
	}
	else if (GD_gui_state(sg) & GUI_STATE_DELETED) {
	    pencolor = late_nnstring(sg, G_deletedpencolor, DEFAULT_DELETEDPENCOLOR);
	    fillcolor = late_nnstring(sg, G_deletedfillcolor, DEFAULT_DELETEDFILLCOLOR);
	    filled = TRUE;
	}
	else if (GD_gui_state(sg) & GUI_STATE_VISITED) {
	    pencolor = late_nnstring(sg, G_visitedpencolor, DEFAULT_VISITEDPENCOLOR);
	    fillcolor = late_nnstring(sg, G_visitedfillcolor, DEFAULT_VISITEDFILLCOLOR);
	    filled = TRUE;
	}
	else {
	    if (((color = agget(sg, "pencolor")) != 0) && color[0])
		pencolor = color;
	    else if (((color = agget(sg, "color")) != 0) && color[0])
		fillcolor = pencolor = color;
	    /* bgcolor is supported for backward compatability */
	    else if (((color = agget(sg, "bgcolor")) != 0) && color[0]) {
		fillcolor = pencolor = color;
	        filled = TRUE;
            }
	    if (((color = agget(sg, "fillcolor")) != 0) && color[0])
		fillcolor = color;
	}
        B2BF(GD_bb(sg), BF);
	if (istyle & ROUNDED) {
	    if (!pencolor) pencolor = DEFAULT_COLOR;
	    if (!fillcolor) fillcolor = DEFAULT_FILL;
	    if (late_int(sg, G_peripheries, 1, 0) || filled) {
		AF[0] = BF.LL;
		AF[1] = BF.UR;
		AF[1].x = AF[2].x;
		AF[1].y = AF[0].y;
		AF[3].x = AF[0].x;
		AF[3].y = AF[2].y;
		round_corners(job, fillcolor, pencolor, AF, 4, istyle);
	    }
	}
	else {
	    if (pencolor)
    		gvrender_set_pencolor(job, pencolor);
	    if (fillcolor)
		gvrender_set_fillcolor(job, fillcolor);
	    if (late_int(sg, G_peripheries, 1, 0))
		gvrender_box(job, BF, filled);
	    else if (filled) { 
		if (fillcolor && fillcolor != pencolor)
		    gvrender_set_pencolor(job, fillcolor);
		gvrender_box(job, BF, filled);
	    }
	}
	if (GD_label(sg))
	    emit_label(job, EMIT_GLABEL, GD_label(sg), (void *) sg);

	if (flags & EMIT_PREORDER) {
	    for (n = agfstnode(sg); n; n = agnxtnode(sg, n)) {
		emit_node(job, n);
		for (e = agfstout(sg, n); e; e = agnxtout(sg, e))
		    emit_edge(job, e);
	    }
	}
	gvrender_end_context(job);
	gvrender_end_cluster(job, g);
	/* when drawing, lay down clusters before sub_clusters */
	if (!(flags & EMIT_CLUSTERS_LAST))
	    emit_clusters(job, sg, flags);
    }
    job->gvc->emit_state = oldstate;
}

static bool is_style_delim(int c)
{
    switch (c) {
    case '(':
    case ')':
    case ',':
    case '\0':
	return TRUE;
    default:
	return FALSE;
    }
}

#define SID 1

static int style_token(char **s, agxbuf * xb)
{
    char *p = *s;
    int token, rc;
    char c;

    while (*p && (isspace(*p) || (*p == ',')))
	p++;
    switch (*p) {
    case '\0':
	token = 0;
	break;
    case '(':
    case ')':
	token = *p++;
	break;
    default:
	token = SID;
	while (!is_style_delim(c = *p)) {
	    rc = agxbputc(xb, c);
	    p++;
	}
    }
    *s = p;
    return token;
}

#define FUNLIMIT 64
static unsigned char outbuf[SMALLBUF];
static agxbuf ps_xb;

static void cleanup(void)
{
    agxbfree(&ps_xb);
}

/* parse_style:
 * This is one of the worse internal designs in graphviz.
 * The use of '\0' characters within strings seems cute but it
 * makes all of the standard functions useless if not dangerous.
 * Plus the function uses static memory for both the array and
 * the character buffer. One hopes all of the values are used
 * before the function is called again.
 */
char **parse_style(char *s)
{
    static char *parse[FUNLIMIT];
    static bool is_first = TRUE;
    int fun = 0;
    bool in_parens = FALSE;
    unsigned char buf[SMALLBUF];
    char *p;
    int c;
    agxbuf xb;

    if (is_first) {
	agxbinit(&ps_xb, SMALLBUF, outbuf);
	atexit(cleanup);
	is_first = FALSE;
    }

    agxbinit(&xb, SMALLBUF, buf);
    p = s;
    while ((c = style_token(&p, &xb)) != 0) {
	switch (c) {
	case '(':
	    if (in_parens) {
		agerr(AGERR, "nesting not allowed in style: %s\n", s);
		parse[0] = (char *) 0;
		agxbfree(&xb);
		return parse;
	    }
	    in_parens = TRUE;
	    break;

	case ')':
	    if (in_parens == FALSE) {
		agerr(AGERR, "unmatched ')' in style: %s\n", s);
		parse[0] = (char *) 0;
		agxbfree(&xb);
		return parse;
	    }
	    in_parens = FALSE;
	    break;

	default:
	    if (in_parens == FALSE) {
		if (fun == FUNLIMIT - 1) {
		    agerr(AGWARN, "truncating style '%s'\n", s);
		    parse[fun] = (char *) 0;
		    agxbfree(&xb);
		    return parse;
		}
		agxbputc(&ps_xb, '\0');	/* terminate previous */
		parse[fun++] = agxbnext(&ps_xb);
	    }
	    agxbput(&ps_xb, agxbuse(&xb));
	    agxbputc(&ps_xb, '\0');
	}
    }

    if (in_parens) {
	agerr(AGERR, "unmatched '(' in style: %s\n", s);
	parse[0] = (char *) 0;
	agxbfree(&xb);
	return parse;
    }
    parse[fun] = (char *) 0;
    agxbfree(&xb);
    (void)agxbuse(&ps_xb);		/* adds final '\0' to buffer */
    return parse;
}

static void emit_job(GVJ_t * job, graph_t * g)
{
    if (!GD_drawing(g)) {
	agerr (AGERR, "layout was not done\n");
	return;
    }

#ifdef WITH_CODEGENS
    Output_file = job->output_file;
    Output_lang = job->output_lang;
#endif

    init_job_flags(job, g);
    init_job_margin(job);
    init_job_dpi(job, g);
    init_job_viewport(job, g);
    init_job_pagination(job, g);

    job->gvc->emit_state = EMIT_GDRAW;
    gvrender_begin_job(job);

    switch (job->output_lang) {
    case EXTENDED_DOT:
        write_extended_dot(job, g, job->output_file);
        break;
    case ATTRIBUTED_DOT:
        write_attributed_dot(g, job->output_file);
        break;
    case CANONICAL_DOT:
        write_canonical_dot(g, job->output_file);
        break;
    case PLAIN:
        write_plain(job, g, job->output_file);
        break;
    case PLAIN_EXT:
        write_plain_ext(job, g, job->output_file);
        break;
    default:
	if (! (job->flags & GVRENDER_X11_EVENTS))
            emit_graph(job, g);
        break;
    }

    /* Flush is necessary because we may be writing to a pipe. */
    if (! job->external_surface && job->output_lang != TK)
        fflush(job->output_file);
}

static FILE *file_select(char *str)
{
    FILE *rv;
    rv = fopen(str, "wb");
    if (rv == NULL) {
        perror(str);
        exit(1);
    }
    return rv;
}

static boxf bezier_bb(bezier bz)
{
    int i;
    point p;
    box bb;
    boxf bbf;

    assert(bz.size > 0);
    bb.LL = bb.UR = bz.list[0];
    for (i = 1; i < bz.size; i++) {
	p=bz.list[i];
        EXPANDBP(bb,p);
    }
    B2BF(bb, bbf);
    return bbf;
}

static void init_splines_bb(splines *spl)
{
    int i;
    bezier bz;
    boxf bb, b;
    pointf p, u;

    assert(spl->size > 0);
    bz = spl->list[0];
    bb = bezier_bb(bz);
    for (i = 0; i < spl->size; i++) {
        if (i > 0) {
            bz = spl->list[i];
            b = bezier_bb(bz);
            EXPANDBB(bb, b);
        }
        if (bz.sflag) {
            P2PF(bz.sp, p);
            P2PF(bz.list[0], u);
            b = arrow_bb(p, u, 1, bz.sflag);
            EXPANDBB(bb, b);
        }
        if (bz.eflag) {
            P2PF(bz.ep, p);
            P2PF(bz.list[bz.size - 1], u);
            b = arrow_bb(p, u, 1, bz.eflag);
            EXPANDBB(bb, b);
        }
    }
    spl->bb = bb;
}

static void init_bb_edge(edge_t *e)
{
    splines *spl;

    spl = ED_spl(e);
    if (spl)
        init_splines_bb(spl);

//    lp = ED_label(e);
//    if (lp)
//        {}
}

static void init_bb_node(graph_t *g, node_t *n)
{
    edge_t *e;

    ND_bb(n).LL.x = ND_coord_i(n).x - ND_lw_i(n);
    ND_bb(n).LL.y = ND_coord_i(n).y - ND_ht_i(n) / 2.;
    ND_bb(n).UR.x = ND_coord_i(n).x + ND_rw_i(n);
    ND_bb(n).UR.y = ND_coord_i(n).y + ND_ht_i(n) / 2.;

    for (e = agfstout(g, n); e; e = agnxtout(g, e))
        init_bb_edge(e);

    /* IDEA - could also save in the node the bb of the node and
    all of its outedges, then the scan time would be proportional
    to just the number of nodes for many graphs.
    Wouldn't work so well if the edges are sprawling all over the place
    because then the boxes would overlap a lot and require more tests,
    but perhaps that wouldn't add much to the cost before trying individual
    nodes and edges. */
}

static void init_bb(graph_t *g)
{
        node_t *n;

	    for (n = agfstnode(g); n; n = agnxtnode(g, n))
		        init_bb_node(g, n);
}

static void auto_output_filename(GVJ_t *job)
{
    static char *buf;
    static int bufsz;
    char gidx[100];  /* large enough for '.' plus any integer */
    char *fn;
    int len;

    if (job->graph_index)
	sprintf(gidx, ".%d", job->graph_index + 1);
    else
	gidx[0] = '\0';
    if (!(fn = job->input_filename))
	fn = "noname.dot";
    len = strlen(fn)			/* typically "something.dot" */
	+ strlen(gidx) 			/* "", ".2", ".3", ".4", ... */
	+ 1 				/* "." */
	+ strlen(job->output_langname)  /* e.g. "png" */
	+ 1; 				/* null terminaor */
    if (bufsz < len) {
	    bufsz = len + 10;
	    buf = realloc(buf, bufsz * sizeof(char));
    }
    strcpy(buf, fn);
    strcat(buf, gidx);
    strcat(buf, ".");
    strcat(buf, job->output_langname);

    job->output_filename = buf;
}

extern gvevent_key_binding_t gvevent_key_binding[];
extern int gvevent_key_binding_size;
extern gvdevice_callbacks_t gvdevice_callbacks;

int gvRenderJobs (GVC_t * gvc, graph_t * g)
{
    GVJ_t *job, *prev_job, *active_job;

    if (!GD_drawing(g)) {
        agerr (AGERR, "Layout was not done.  Missing layout plugins? \n");
        return -1;
    }

    init_bb(g);
    init_gvc(gvc, g);
    init_layering(gvc, g);

    gvc->keybindings = gvevent_key_binding;
    gvc->numkeys = gvevent_key_binding_size;
    prev_job = NULL;
    for (job = gvjobs_first(gvc); job; job = gvjobs_next(gvc)) {
	if (gvc->gvg) {
	    job->input_filename = gvc->gvg->input_filename;
	    job->graph_index = gvc->gvg->graph_index;
	}
	else {
	    job->input_filename = NULL;
	    job->graph_index = 0;
	}
	job->common = &(gvc->common);
	job->layout_type = gvc->layout.type;
	job->bb = gvc->bb;

        job->output_lang = gvrender_select(job, job->output_langname);
        if (job->output_lang == NO_SUPPORT) {
            agerr (AGERR, "renderer for %s is unavailable\n", job->output_langname);
            return -1;
        }

	/* if we already have an active job list and the device doesn't support mutiple output files, or we are about to write to a different output device */
        if ((active_job = gvc->active_jobs)
	    && (!(active_job->flags & GVRENDER_DOES_MULTIGRAPH_OUTPUT_FILES)
	      || (strcmp(job->output_langname,active_job->output_langname)))) {

	    gvrender_end_job(active_job);
            gvdevice_finalize(gvc); /* finalize previous jobs */
	    
            gvc->active_jobs = NULL; /* clear active list */
	    gvc->common.viewNum = 0;
	    prev_job = NULL;
        }

        if (!job->output_file) {        /* if not yet opened */
	    if (gvc->common.auto_outfile_names)
		auto_output_filename(job);
            if (job->output_filename)
                job->output_file = file_select(job->output_filename);
            else
                job->output_file = stdout;
        }

	if (prev_job)
            prev_job->next_active = job;  /* insert job in active list */
	else
	    gvc->active_jobs = job;   /* first job of new list */
	job->next_active = NULL;      /* terminate active list */
	prev_job = job;

	job->callbacks = &gvdevice_callbacks;

        emit_job(job, g);

        /* the last job, after all input graphs are processed,
         *      is finalized from gvFreeContext()
         */
    }
    return 0;
}
