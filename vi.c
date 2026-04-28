#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vi.h"
#include "conf.c"
#include "ex.c"
#include "lbuf.c"
#include "led.c"
#include "regex.c"
#include "ren.c"
#include "term.c"
#include "uc.c"

int vi_hidch;			/* show hidden chars */
int vi_lncol;			/* line numbers cursor offset */
static int vi_lnnum;		/* line numbers */
/* screen redraw - bit 1: whole screen, bit 2: current line, bit 3: update vi_col */
static int vi_mod;
static char vi_word_m[] = "\0leEwW";	/* line word navigation */
static char *vi_word = vi_word_m;
static char *_vi_word = vi_word_m;
static int vi_wsel = 1;
static int vi_rshift;			/* row shift for vi_word */
static int vi_arg;			/* numeric argument */
static char vi_charlast[5];		/* the last character searched via f, t, F, or T */
static int vi_charcmd;			/* the character finding command */
static int vi_ybuf;			/* current yank buffer */
static int vi_col;			/* the column requested by | command */
static int vi_scrollud;			/* scroll amount for ^u and ^d */
static int vi_scrolley;			/* scroll amount for ^e and ^y */
static int vi_cndir = 1;		/* ^n direction */
static int vi_status;			/* permanent status bar */
static int vi_tsm;			/* type of the status message */
static int vi_nlword;			/* new line mode for eEwWbB */
static int vi_visual;			/* visual mode: 0=off, 'v'=char, 'V'=line */
static int vi_vrow;			/* selection anchor row */
static int vi_voff;			/* selection anchor column */

void *emalloc(size_t size)
{
	void *p;
	if (!(p = malloc(size))) {
		fprintf(stderr, "\nmalloc: out of memory\n");
		exit(EXIT_FAILURE);
	}
	return p;
}

void *erealloc(void *p, size_t size)
{
	if (!(p = realloc(p, size))) {
		fprintf(stderr, "\nrealloc: out of memory\n");
		exit(EXIT_FAILURE);
	}
	return p;
}

static void reverse_in_place(char *str, int len)
{
	char *p1 = str;
	char *p2 = str + len - 1;
	while (p1 < p2) {
		char tmp = *p1;
		*p1++ = *p2;
		*p2-- = tmp;
	}
}

char *itoa(int n, char s[])
{
	int i = 0, sign;
	if ((sign = n) < 0)		/* record sign */
		n = -n;			/* make n positive */
	do {				/* generate digits in reverse order */
		s[i++] = n % 10 + '0';	/* get next digit */
	} while ((n /= 10) > 0);	/* delete it */
	if (sign < 0)
		s[i++] = '-';
	s[i] = '\0';
	reverse_in_place(s, i);
	return &s[i];
}

static void vi_drawmsg(char *msg)
{
	syn_blockhl = -1;
	preserve(int, xtd, xtd = 2;)
	preserve(int, ftidx,)
	syn_setft(bar_ft);
	RS(2, led_crender(msg, xrows, 0, 0, xcols))
	restore(xtd)
	restore(ftidx)
}
#define vi_drawmsg_mpt(msg) { vi_drawmsg(msg); if (!xmpt) xmpt = 1; }

static int vi_nextcol(char *ln, int dir, int *off)
{
	int o = ren_off(ln, ren_next(ln, ren_pos(ln, *off), dir));
	if (*rstate->chrs[o] == '\n')
		return -1;
	*off = o;
	return 0;
}

#define vi_drawnum(func) \
{ \
nrow = xrow; \
noff = xoff; \
for (i = 0, ret = 0;; i++) { \
	l1 = ren_next(c, ren_pos(c, noff), 1)-1-xleft+vi_lncol; \
	if (l1 > xcols || l1 < 0 || ret || l1 >= rstate->cmax + vi_lncol) \
		break; \
	i = i > 99 ? i % 100 : i; \
	itoa(i%10 ? i%10 : i, snum); \
	tmp[l1] = *snum; \
	ret = func; \
} } \

static void vi_visual_attrib(char *s, int row)
{
	if (!vi_visual || !s)
		return;
	int ar = vi_vrow, ao = vi_voff;
	int cr = xrow,   co = xoff;
	if (ar > cr || (ar == cr && ao > co)) {
		swap(&ar, &cr);
		swap(&ao, &co);
	}
	if (row < ar || row > cr)
		return;
	if (!led_attsb)
		sbuf_make(led_attsb, sizeof(led_att) * 16)
	led_att la;
	la.att = SYN_RV;
	la.s   = s;
	ren_state *r = ren_position(s);
	int o_beg, o_end;
	if (vi_visual == 'V') {
		o_beg = 0;
		o_end = r->n - 1;
	} else if (vi_visual == 'b') {
		o_beg = MIN(vi_voff, xoff);
		o_end = MAX(vi_voff, xoff);
	} else if (ar == cr) {		/* single row, char visual */
		o_beg = ao; o_end = co;
	} else if (row == ar) {		/* first row: anchor to EOL */
		o_beg = ao; o_end = lbuf_eol(xb, row, 1);
	} else if (row == cr) {		/* last row: 0 to cursor */
		o_beg = 0;  o_end = co;
	} else {			/* middle row: full line */
		o_beg = 0;  o_end = lbuf_eol(xb, row, 1);
	}
	o_beg = MAX(0, MIN(o_beg, r->n - 1));
	o_end = MAX(0, MIN(o_end, r->n - 1));
	for (int o = o_beg; o <= o_end; o++) {
		la.off = o;
		sbuf_mem(led_attsb, &la, (int)sizeof(la))
	}
}

static void vi_drawrow(int row)
{
	int l1, i, i1, lnnum = vi_lnnum;
	char *c, *s;
	static char ch[5] = "~";
	if (xmpt == 1 && !vi_status && row == xtop + xrows - 1)
		return;
	if (*vi_word && xled) {
		int noff, nrow, ret;
		c = lbuf_get(xb, xrow);
		if (row != xrow+1 || !c || *c == '\n') {
			vi_rshift = (row > xrow+1 && c && *c != '\n');
			s = lbuf_get(xb, row - vi_rshift);
			goto skip;
		}
		char tmp[xcols+3], snum[32];
		memset(tmp, ' ', xcols+1);
		tmp[xcols+1] = '\n';
		tmp[xcols+2] = '\0';
		i1 = isupper((unsigned char)*vi_word);
		if (*vi_word == 'e' || *vi_word == 'E')
			vi_drawnum(lbuf_wordend(xb, i1, 2, &nrow, &noff))
		else if (*vi_word == 'w' || *vi_word == 'W')
			vi_drawnum(lbuf_wordbeg(xb, i1, 2, &nrow, &noff))
		if (*vi_word == 'l') {
			vi_drawnum(vi_nextcol(c, 1, &noff))
			vi_drawnum(vi_nextcol(c, -1, &noff))
		} else
			vi_drawnum(lbuf_wordend(xb, i1, -2, &nrow, &noff))
		tmp[ren_next(c, ren_pos(c, xoff), 1)-1-xleft+vi_lncol] = *vi_word;
		preserve(int, xorder, xorder = 0;)
		preserve(int, syn_blockhl, syn_blockhl = -1;)
		preserve(int, xtd, xtd = dir_context(c) * 2;)
		preserve(int, ftidx,)
		syn_setft(n_ft);
		RS(2, led_crender(tmp, row - xtop, 0, 0, xcols))
		restore(xorder)
		restore(syn_blockhl)
		restore(xtd)
		restore(ftidx)
		return;
	}
	s = lbuf_get(xb, row);
	skip:
	rstate += row != xrow;
	if (!s)
		s = row ? ch : ch+1;
	else if (lnnum && xled) {
		char tmp[32], tmp1[32], *p;
		c = tmp, i = 0, i1 = 0;
		if (lnnum == 1 || lnnum & 2) {
			c = itoa(row+1-vi_rshift, tmp);
			*c++ = ' ';
			i = itoalen(xtop+xrows);
		}
		p = c;
		if (lnnum == 1 || lnnum & 4 || lnnum & 8) {
			c = itoa(abs(xrow-row+vi_rshift), c);
			*c++ = ' ';
			i1 = itoalen(xrows);
		}
		*c = '\0';
		l1 = (c - tmp) + (i+i1 - (strlen(tmp) - !!i - !!i1));
		vi_lncol = dir_context(s) < 0 ? 0 : l1;
		memset(c, ' ', l1 - (c - tmp));
		c[l1 - (c - tmp)] = '\0';
		vi_visual_attrib(s, row);
		led_crender(s, row - xtop, l1, xleft, xleft + xcols - l1)
		preserve(int, syn_blockhl, syn_blockhl = -1;)
		preserve(int, ftidx,)
		syn_setft(nn_ft);
		if ((lnnum == 1 || lnnum & 4) && !xleft && vi_lncol) {
			for (i1 = 0; i1 < rstate->cmax &&
					memchr(" \t", *rstate->chrs[ren_off(s, i1)], 2);)
				i1 = ren_next(s, i1, 1);
			i1 -= (itoa(abs(xrow-row+vi_rshift), tmp1) - tmp1)+1;
			if (i1 >= 0) {
				memset(p, ' ', strlen(p));
				RS(2, led_prender(tmp1, row - xtop, l1+i1, 0, l1))
			}
		}
		RS(2, led_prender(tmp, row - xtop, 0, 0, l1))
		restore(syn_blockhl)
		restore(ftidx)
		return;
	}
	vi_visual_attrib(s, row);
	led_crender(s, row - xtop, 0, xleft, xleft + xcols)
	rstate = rstates;
}

/* redraw the screen */
static void vi_drawagain(int i)
{
	syn_scdir(0);
	for (; i < xtop + xrows; i++)
		vi_drawrow(i);
}

/* update the screen */
static void vi_drawupdate(int i)
{
	int n;
	term_pos(0, 0);
	term_room(i);
	syn_scdir(i);
	if (i < 0) {
		n = MIN(-i, xrows);
		for (i = 0; i < n; i++)
			vi_drawrow(xtop + xrows - n + i);
	} else {
		n = MIN(i, xrows);
		for (i = n-1; i >= 0; i--)
			vi_drawrow(xtop + i);
	}
}

static char *vi_prompt(char *msg, char *ft, char *insert, int *ret, int *kmap, int *mlen)
{
	sbuf_smake(sb, xcols)
	sbuf_str(sb, msg)
	*mlen = sb->s_n;
	term_pos(xrows, 0);
	syn_setft(ft);
	*ret = led_prompt(sb, insert, kmap, NULL, 0, 1) == '\n';
	syn_setft(xb_ft);
	return sb->s;
}

static char *vi_enprompt(char *msg, char *insert, int *ret, int *mlen)
{
	int kmap = 0;
	return vi_prompt(msg, ex_ft, insert, ret, &kmap, mlen);
}

static int vi_yankbuf(void)
{
	int c = term_read(TK_CTL('l'));
	if (c == '"')
		return term_read(TK_CTL('l'));
	term_dec()
	return 0;
}

static int vi_prefix(void)
{
	int n = 0;
	int c = term_read(TK_CTL('l'));
	if (c >= '1' && c <= '9') {
		while (c >= '0' && c <= '9') {
			n = n * 10 + c - '0';
			c = term_read(TK_CTL('l'));
		}
	}
	return n;
}

static int vi_digit(void)
{
	int c = term_read(0);
	if (c >= '0' && c <= '9')
		return c - '0';
	return -1;
}

static int vi_off2col(struct lbuf *lb, int row, int off)
{
	char *ln = lbuf_get(lb, row);
	return ln ? ren_pos(ln, off) : 0;
}

static int vi_col2off(struct lbuf *lb, int row, int col)
{
	char *ln = lbuf_get(lb, row);
	if (!ln)
		return 0;
	ren_state *r = ren_position(ln);
	if (col >= r->cmax)
		return r->col[r->cmax - 1];
	return r->col[col];
}

static int vi_search(int cmd, int cnt, int *row, int *off, int msg)
{
	int i, dir, ret;
	char vi_msg[512];
	if (cmd == '/' || cmd == '?') {
		char sign[4] = {cmd};
		char *kw = vi_prompt(sign, vs_ft, NULL, &ret, &xkmap, &i);
		vi_drawmsg_mpt(kw)
		if (!ret) {
			free(kw);
			return 1;
		}
		ex_krsset(kw + i, cmd == '/' ? +2 : -2);
		if (!xkwdrs)
			vi_drawmsg_mpt("syntax error")
		free(kw);
	} else if (msg)
		ex_krsset(xregs['/'] ? xregs['/']->s : NULL, xkwddir);
	if (!lbuf_len(xb) || (!xkwddir || !xkwdrs))
		return 1;
	dir = cmd == 'N' ? -xkwddir : xkwddir;
	for (i = 0; i < cnt; i++) {
		if (lbuf_search(xb, xkwdrs, dir, 0, lbuf_len(xb),
				msg ? dir : -1, 1, row, off)) {
			if (msg) {
				snprintf(vi_msg, sizeof(vi_msg), "\"%s\" not found %d/%d",
						xregs['/'] ? xregs['/']->s : "", i, cnt);
				vi_drawmsg_mpt(vi_msg)
			}
			return 1;
		}
	}
	return 0;
}

/* read a line motion */
static int vi_motionln(int *row, int cmd, int cnt)
{
	int mark, c = term_read(TK_CTL('l'));
	switch (c) {
	case '\033':	/* Arrow keys */
		c = term_read(0);
		if (c == '[') {
			c = term_read(0);
			switch (c) {
			case 'A':	/* ↑ */
				*row = MAX(*row - cnt, 0);
				c = 'k';
				break;
			case 'B':	/* ↓ */
				*row = MIN(*row + cnt, lbuf_len(xb) - 1);
				c = 'j';
				break;
			default:	/* Not a line motion so we put back all the arrow characters */
				term_back('\033');
				term_back('[');
				term_back(c);
				return 0;
			}
		} else	/* Not an arrow sequence so we abort */
			return 0;
		break;
	case '\n':
	case '+':
	case 'j':
		*row = MIN(*row + cnt, lbuf_len(xb) - 1);
		break;
	case 'k':
	case '-':
		*row = MAX(*row - cnt, 0);
		break;
	case '\'':
		if ((mark = term_read(0)) <= 0)
			return -1;
		if (lbuf_jump(xb, mark, row, &mark))
			return -1;
		break;
	case 'G':
		*row = vi_arg ? cnt - 1 : lbuf_len(xb) - 1;
		break;
	case 'H':
		*row = MIN(xtop + cnt - 1, lbuf_len(xb) - 1);
		break;
	case 'L':
		*row = MIN(xtop + xrows - 1 - cnt + 1, lbuf_len(xb) - 1);
		break;
	case 'M':
		*row = MIN(xtop + xrows / 2, lbuf_len(xb) - 1);
		break;
	default:
		if (c == cmd) {
			*row = MIN(*row + cnt - 1, lbuf_len(xb) - 1);
			break;
		}
		if (c == '%' && vi_arg) {
			if (cnt > 100)
				return -1;
			*row = lbuf_len(xb) * cnt / 100;
			break;
		}
		term_dec()
		return 0;
	}
	if (*row < 0)
		*row = 0;
	return c;
}

static char *vi_curword(struct lbuf *lb, int row, int off, int n, int ex)
{
	char *ln = lbuf_get(lb, row);
	if (!ln || !n)
		return NULL;
	off = ren_noeol(ln, off);
	char **chrs = rstate->chrs;
	int cap = rstate->n;
	int end = off;
	for (int i = 0; i < n && end < cap; i++)
		while (uc_kind(chrs[end++]) == 1);
	for (; off > 0 && uc_kind(chrs[off - 1]) == 1; off--);
	if (!end || --end == off)
		return NULL;
	sbuf_smake(sb, 64)
	if (n <= 1) {
		sbuf_str(sb, "\\<")
		sbuf_mem(sb, chrs[off], chrs[end] - chrs[off])
		sbuf_str(sb, "\\>")
	} else
		ex_regesc(sb, chrs[off], chrs[end], ex);
	sbufn_ret(sb, sb->s)
}

static void vi_regput(int c, const char *s, int lnmode)
{
	if (lnmode) {
		sbuf *i_s;
		for (int i = 8; i > 0; i--)
			if ((i_s = xregs['0'+i]))
				ex_regput('0' + i + 1, i_s->s, 0);
		ex_regput('1', s, 0);
	} else if (xregs[c])
		ex_regput('0', xregs[c]->s, 0);
	ex_regput(tolower(c), s, isupper(c));
}

rset *fsincl;
static int fspos;
static int fsdir;

void dir_calc(char *path)
{
	struct dirent *dirp;
	struct stat statbuf;
	int i = 0, ret;
	char *cpath, *ptrs[1024];
	int plen[1024];
	DIR *dp, *sdp, *dps[1024];
	unsigned int pathlen = strlen(path), len;
	if (!(dp = opendir(path)))
		return;
	cpath = emalloc(pathlen + 1024);
	strcpy(cpath, path);
	sbuf_smake(sb, 1024)
	temp_pos(1, -1, 0, 0);
	fspos = 0;
	for (;;) {
		while ((dirp = readdir(dp))) {
			len = strlen(dirp->d_name)+1;
			if (strcmp(dirp->d_name, ".") == 0 ||
				strcmp(dirp->d_name, "..") == 0 ||
				len > 1023)
				continue;
			cpath[pathlen] = '/';
			memcpy(&cpath[pathlen+1], dirp->d_name, len);
			ret = lstat(cpath, &statbuf);
			if (ret >= 0 && S_ISDIR(statbuf.st_mode)) {
				if (!(sdp = opendir(cpath)) || i >= LEN(ptrs))
					break;
				dps[i] = sdp;
				ptrs[i] = cpath;
				cpath = emalloc(pathlen + 1024);
				memcpy(cpath, ptrs[i], pathlen + len);
				plen[i++] = pathlen + len;
			} else if (ret >= 0 && S_ISREG(statbuf.st_mode))
				if (!fsincl || rset_match(fsincl, cpath, 0)) {
					sbuf_mem(sb, cpath, (int)(pathlen + len))
					sbuf_chr(sb, '\n')
				}
		}
		closedir(dp);
		free(cpath);
		if (i > 0) {
			dp = dps[--i];
			pathlen = plen[i];
			cpath = ptrs[i];
		} else
			break;
	}
	sbuf_null(sb)
	if (sb->s_n > 1)
		temp_write(1, sb->s);
	free(sb->s);
}

#define fssearch() \
len = lbuf_s(path)->len; \
path[len] = '\0'; \
ret = ex_edit(path, len); \
path[len] = '\n'; \
if (ret && xrow) { \
	*row = xrow; *off = xoff; /* short circuit */ \
	if (!vi_search('n', cnt, row, off, 0)) \
		return 1; \
	++*off; \
} else { \
	*row = 0; *off = 0; \
} \
if (!vi_search(*row ? 'N' : 'n', cnt, row, off, 0)) \
	return 1; \

static int fs_search(int cnt, int *row, int *off)
{
	char *path;
	int again = 0, ret, len;
	wrap:
	while (fspos < lbuf_len(tempbufs[1].lb)) {
		path = tempbufs[1].lb->ln[fspos++];
		fssearch()
	}
	if (fspos == lbuf_len(tempbufs[1].lb) && !again) {
		fspos = 0;
		again = 1;
		goto wrap;
	}
	return 0;
}

static int fs_searchback(int cnt, int *row, int *off)
{
	char *path;
	int ret, len;
	while (--fspos >= 0) {
		path = tempbufs[1].lb->ln[fspos];
		fssearch()
	}
	return 0;
}

static void vc_status(int type)
{
	int l, col;
	unsigned int cp;
	char cbuf[8] = "", vi_msg[512], *c;
	col = vi_off2col(xb, xrow, xoff);
	col = ren_cursor(lbuf_get(xb, xrow), col) + 1;
	char *vs = vi_visual == 'V' ? "-- VISUAL LINE -- " :
		   vi_visual == 'b' ? "-- VISUAL BLOCK -- " :
		   vi_visual ? "-- VISUAL -- " : "";
	if (type && lbuf_get(xb, xrow)) {
		c = rstate->chrs[xoff];
		uc_code(cp, c, l)
		memcpy(cbuf, c, l);
		snprintf(vi_msg, sizeof(vi_msg), "<%s> 0x%x 0%o %u %dL %dW S%td O%d C%d %s",
			cbuf, cp, cp, cp, l, rstate->wid[xoff], c - lbuf_get(xb, xrow),
			xoff, col, vs);
	} else {
		snprintf(vi_msg, sizeof(vi_msg),
			"\"%s\"%s%dL %d%% L%d C%d B%td %s",
			xb_path[0] ? xb_path : "unnamed",
			xb->modified ? "* " : " ", lbuf_len(xb),
			xrow * 100 / MAX(1, lbuf_len(xb)-1), xrow+1, col,
			istempbuf(ex_buf) ? tempbufs - ex_buf - 1 : ex_buf - bufs, vs);
	}
	vi_drawmsg_mpt(vi_msg)
}

/* read a motion */
static int vi_motion(int vc, int *row, int *off)
{
	static sbuf *savepath[10];
	static rset *bre;
	static int srow[10], soff[10], lkwdcnt;
	static int cadir = 1;
	char *cs;
	int cnt = vi_arg ? vi_arg : 1;
	int mv, i, dir, mark;

	if ((mv = vi_motionln(row, 0, cnt))) {
		*off = -1;
		return mv;
	}
	mv = term_read(TK_CTL('l'));
	switch (mv) {
	case '\033':	/* Arrow keys */
		mv = term_read(0);
		if (mv == '[') {
			if (!(cs = lbuf_get(xb, *row)))
				return -1;
			dir = dir_context(lbuf_get(xb, *row));
			mv = term_read(0);
			switch (mv) {
			case 'D':	/* ← */
				dir = -dir;
			case 'C':	/* → */
				for (i = 0; i < cnt; i++)
					if (vi_nextcol(cs, dir, off))
						break;
				break;
			default:	/* Not a motion managed by this function so we abort */
				return 0;
			}
		} else	/* Not a 033[X command so we abort */
			return 0;
		break;
	case ',':
	case ';':
		if (!vi_charlast[0])
			return -1;
		if (mv == ',')
			mv = vi_charcmd == 'F' || vi_charcmd == 'T'
				? tolower(vi_charcmd) : toupper(vi_charcmd);
		else
			mv = vi_charcmd;
		if (lbuf_findchar(xb, vi_charlast, mv, cnt, row, off))
			return -1;
		break;
	case 'h':
	case 'l':
		if (!(cs = lbuf_get(xb, *row)))
			return -1;
		dir = dir_context(cs);
		dir = mv == 'h' ? -dir : dir;
		for (i = 0; i < cnt; i++)
			if (vi_nextcol(cs, dir, off))
				break;
		break;
	case ' ':
	case 127:
	case TK_CTL('h'):
		dir = mv == ' ' ? +1 : -1;
		cs = lbuf_get(xb, *row);
		mark = cs ? ren_position(cs)->n : 0;
		i = *off;
		*off += cnt * dir;
		if (*off < 0 || *off >= mark) {
			cnt -= dir > 0 ? mark - i : i;
			*off = dir > 0 ? mark : 0;
			while ((cs = lbuf_get(xb, *row + dir))) {
				*row += dir;
				mark = uc_slen(cs);
				if (cnt - mark <= 0) {
					*off = dir < 0 ? mark - cnt : cnt;
					break;
				}
				cnt -= mark;
			}
		}
		if (!vc && dir > 0 && lbuf_get(xb, *row + dir)
				&& (mark < 2 || *off >= mark - 1)) {
			*row += dir;
			*off = 0;
		}
		break;
	case 'f':
	case 'F':
	case 't':
	case 'T':
		if (!(cs = led_read(&xkmap, term_read(0))))
			return -1;
		strcpy(vi_charlast, cs);
		vi_charcmd = mv;
		if (lbuf_findchar(xb, cs, mv, cnt, row, off))
			return -1;
		break;
	case 'b':
	case 'B':
		mark = mv == 'B';
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, mark, -(vi_nlword+1), row, off))
				break;
		break;
	case 'e':
	case 'E':
		mark = mv == 'E';
		for (i = 0; i < cnt; i++)
			if (lbuf_wordend(xb, mark, vi_nlword+1, row, off))
				break;
		break;
	case 'w':
	case 'W':
		mark = mv == 'W';
		if (vc && cnt == 1)
			dir = 2;
		else
			dir = vi_nlword+1;
		if (vc == 'c') {
			/* vim: cw/cW acts like ce/cE (no trailing whitespace) */
			int prow = *row, poff = *off;
			for (i = 0; i < cnt; i++)
				if (lbuf_wordend(xb, mark, dir, row, off))
					break;
			if (prow == *row)
				return mv == 'w' ? 'e' : 'E';
			*row = prow;
			*off = poff;
		}
		for (i = 0; i < cnt; i++)
			if (lbuf_wordbeg(xb, mark, dir, row, off))
				break;
		break;
	case '(':
	case ')':
		dir = mv == '(' ? 1 : -1;
		if (!bre)
			bre = rset_smake("^[.?!]+['\\])]*(?:[ \t]+\n?|\n)", 0);
		int subs[2], org;
		for (i = 0; i < cnt; i++) {
			mark = *row;
			org = *off;
			for (; (cs = lbuf_get(xb, *row)) && *cs == '\n'; *row += dir);
			if (*row != mark) {
				*off = MAX(0, lbuf_indents(xb, *row));
				if (dir > 0)
					continue;
				*off = lbuf_eol(xb, *row, 1);
			}
			while (!lbuf_next(xb, dir, row, off)) {
				cs = rstate->chrs[*off];
				if (*off == 0 && *cs == '\n') {
					if (dir < 0 && (mark - *row) > 1)
						*row += 1;
					*off = MAX(0, lbuf_indents(xb, *row));
					break;
				} else if (rset_find(bre, cs, subs, 0) >= 0) {
					if (mark == *row && rstate->chrs[org] == cs + subs[1])
						continue;
					if (!cs[subs[1]]) {
						if (dir < 0 && *row + 1 == mark)
							continue;
						*row += 1;
						*off = MAX(0, lbuf_indents(xb, *row));
					} else
						*off += uc_off(cs, subs[1]);
					break;
				}
			}
		}
		return mv;
	case '{':
	case '}':
	case '[':
	case ']':
		dir = mv == '{' || mv == '[' ? 1 : -1;
		mark = mv == '[' || mv == ']' ? '\n' : '{';
		for (i = 0; i < cnt; i++)
			if (lbuf_sectionbeg(xb, dir, row, off, mark))
				break;
		break;
	case TK_CTL(']'):	/* this is also ^5 on some systems */
	case TK_CTL('p'):
		#define open_saved(n) \
		if (savepath[n]) { \
			*row = srow[n]; *off = soff[n]; \
			ex_edit(savepath[n]->s, savepath[n]->s_n); \
		} \

		if (vi_arg && (cs = vi_curword(xb, *row, *off, cnt, 0))) {
			ex_krsset(cs, +1);
			free(cs);
		}
		struct buf* tmpex_buf = istempbuf(ex_buf) ? ex_pbuf : ex_buf;
		if (mv == TK_CTL(']')) {
			if (vi_arg || lkwdcnt != xkwdcnt)
				term_exec("", 1, '&')
			lkwdcnt = xkwdcnt;
			fspos += fsdir < 0 ? 1 : 0;
			fspos = MIN(fspos, lbuf_len(tempbufs[1].lb));
			fs_search(1, row, off);
			fsdir = 1;
		} else {
			fspos -= fsdir > 0 ? 1 : 0;
			if (!fs_searchback(1, row, off)) {
				open_saved(0)
				fsdir = 0;
			} else
				fsdir = -1;
			fspos = MAX(fspos, 0);
		}
		if (tmpex_buf != ex_buf)
			ex_pbuf = tmpex_buf;
		bsync_ret:
		for (i = xbufcur-1; i >= 0 && bufs[i].mtime == -1; i--)
			ex_bufpostfix(&bufs[i], 1);
		syn_setft(xb_ft);
		vc_status(0);
		xtop = MAX(0, *row - xrows / 2);
		vi_mod |= 1;
		break;
	case TK_CTL('t'):
		if (vi_arg % 2 == 0 && vi_arg < LEN(srow) * 2) {
			vi_arg /= 2;
			if (!savepath[vi_arg])
				sbuf_make(savepath[vi_arg], 128)
			sbuf_cut(savepath[vi_arg], 0)
			sbufn_str(savepath[vi_arg], xb_path)
			srow[vi_arg] = *row; soff[vi_arg] = *off;
			break;
		}
		open_saved(vi_arg / 2)
		goto bsync_ret;
	case '0':
		*off = 0;
		break;
	case '^':
		*off = lbuf_indents(xb, *row);
		break;
	case '$':
		*off = lbuf_eol(xb, *row, 1);
		break;
	case '|':
		vi_col = cnt - 1;
		break;
	case '/':
	case '?':
	case 'n':
	case 'N':
		if (vi_search(mv, cnt, row, off, 1))
			return -1;
		xtop = MAX(0, *row - xrows / 2);
		vi_mod |= mv == '/' || mv == '?';
		break;
	case '*':
	case TK_CTL('a'):
		if (mv == TK_CTL('a') || vi_arg) {
			if (!(cs = vi_curword(xb, *row, *off, cnt, 0)))
				return -1;
			ex_krsset(cs, +1);
			free(cs);
		}
		if (vi_search(cadir < 0 ? 'N' : 'n', 1, row, off, 1))
			cadir = -cadir;
		else if (*row < xtop || *row >= xtop + xrows - 1)
			xtop = MAX(0, *row - xrows / 2);
		break;
	case '`':
		if ((mark = term_read(0)) <= 0)
			return -1;
		if (lbuf_jump(xb, mark, row, off))
			return -1;
		break;
	case '%':
		if (lbuf_pair(xb, row, off))
			return -1;
		break;
	default:
		return 0;
	}
	return mv;
}

static void vi_yank(int r1, int o1, int r2, int o2, int lnmode)
{
	sbuf rsb;
	lbuf_region(xb, &rsb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf, rsb.s, lnmode);
	free(rsb.s);
	xrow = r1;
	xoff = lnmode ? xoff : o1;
}

static void vi_delete(int r1, int o1, int r2, int o2, int lnmode)
{
	sbuf rsb;
	lbuf_region(xb, &rsb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	vi_regput(vi_ybuf, rsb.s, lnmode);
	free(rsb.s);
	if (lnmode)
		lbuf_edit(xb, NULL, r1, r2 + 1, 0, 0);
	else {
		rsb.s = "";
		rsb.s_n = 0;
		char *s = lbuf_joinsb(xb, r1, r2, &rsb, &o1, &o2);
		lbuf_edit(xb, s, r1, r2 + 1, o1, o1);
		free(s);
	}
	xrow = r1;
	xoff = lnmode ? lbuf_indents(xb, xrow) : o1;
}

static void vi_indents(char *ln, int *l)
{
	if (!xai || !ln)
		ln = "";
	char *pln = ln;
	for (; *ln == ' ' || *ln == '\t'; ln++);
	*l = ln - pln;
}

static int lmodified;

static int vi_change(int r1, int o1, int r2, int o2, int lnmode)
{
	char *post, *ln = lbuf_get(xb, r1);
	sbuf rsb;
	int key, tlen, l1, l2 = 1, postn = 1;
	sbuf_smake(sb, xcols)
	if (lnmode || !ln) {
		vi_indents(ln, &l1);
		o1 = l1;
		post = "\n";
		tlen = -1;
		lbuf_region(xb, &rsb, r1, 0, r2, -1);
	} else {
		l1 = uc_chr(ln, o1) - ln;
		post = uc_chr(lbuf_get(xb, r2), o2);
		l2 = uc_chrn(post, -1, &postn) - post;
		tlen = lbuf_s(ln)->len+1;
		lbuf_region(xb, &rsb, r1, o1, r2, o2);
	}
	vi_regput(vi_ybuf, rsb.s, lnmode);
	free(rsb.s);
	term_pos(r1 - xtop < 0 ? 0 : r1 - xtop, 0);
	term_room(r1 < xtop ? xtop - xrow : r1 - r2 -
			(*vi_word && ln && *ln != '\n' && r1 != r2));
	xrow = r1;
	if (r1 < xtop)
		xtop = r1;
	sbuf_mem(sb, ln, l1)
	key = led_input(sb, post, postn, r1 - (r1 - r2), 0, &postn);
	if (postn + l2 != tlen || memcmp(ln + l1, sb->s + l1, tlen - l2 - l1))
		lbuf_edit(xb, sb->s, r1, r2 + 1, o1, xoff);
	free(sb->s);
	lmodified = 1;
	return key;
}

static void vi_case(int r1, int o1, int r2, int o2, int lnmode, int cmd)
{
	sbuf rsb;
	lbuf_region(xb, &rsb, r1, lnmode ? 0 : o1, r2, lnmode ? -1 : o2);
	char *s = rsb.s;
	while (uc_len(s)) {
		int c = (unsigned char) s[0];
		if (c <= 0x7f) {
			if (cmd == 'u')
				s[0] = tolower(c);
			if (cmd == 'U')
				s[0] = toupper(c);
			if (cmd == '~')
				s[0] = islower(c) ? toupper(c) : tolower(c);
		}
		s += uc_len(s);
	}
	if (lnmode) {
		lbuf_edit(xb, rsb.s, r1, r2 + 1, 0, 0);
		free(rsb.s);
	} else {
		s = lbuf_joinsb(xb, r1, r2, &rsb, &o1, &o2);
		free(rsb.s);
		lbuf_edit(xb, s, r1, r2 + 1, o1, o2);
		free(s);
	}
	xrow = r2;
	xoff = lnmode ? lbuf_indents(xb, r2) : o2;
}

static void vi_pipe(int r1, int r2)
{
	int mlen, ret;
	char region[64], *p = region;
	if (r1 == r2 && !vi_arg)
		*p++ = '.';
	else {
		p = itoa(r1+1, region);
		*p++ = ',';
		p = itoa(r2+1, p);
	}
	*p++ = '!';
	*p = '\0';
	char *cmd = vi_enprompt(":", region, &ret, &mlen);
	if (ret)
		ex_command(cmd + mlen)
	if (!xmpt)
		vi_drawmsg_mpt(cmd)
	free(cmd);
}

static void vi_shift(int r1, int r2, int dir, int count)
{
	sbuf_smake(sb, 1024)
	char *ln;
	int i, c;
	for (i = r1; i <= r2; i++) {
		if (!(ln = lbuf_get(xb, i)))
			continue;
		for (c = 0; c < count; c++) {
			if (dir < 0) {
				if (*ln != ' ' && *ln != '\t')
					break;
				ln++;
			} else if (*ln != '\n' || r1 == r2)
				sbuf_chr(sb, '\t')
		}
		sbufn_str(sb, ln)
		lbuf_edit(xb, sb->s, i, i + 1, 0, 0);
		sbuf_cut(sb, 0)
	}
	xoff = lbuf_indents(xb, r1);
	free(sb->s);
}

static int vc_insert(int cmd);

static int vc_block_insert(int vcmd, int r1, int r2, int col)
{
	int eol = lbuf_eol(xb, r1, 0);
	xrow = r1;
	xoff = MIN(col, eol);
	char *old_ln = lbuf_get(xb, r1);
	int old_nbytes = old_ln ? lbuf_s(old_ln)->len : 0;
	char *snap = emalloc(old_nbytes + 2);
	if (old_ln) {
		memcpy(snap, old_ln, old_nbytes);
		snap[old_nbytes] = '\n';
	}
	snap[old_nbytes + (old_ln ? 1 : 0)] = '\0';
	int key = vc_insert(vcmd == 'A' ? 'a' : 'i');
	char *new_ln = lbuf_get(xb, r1);
	int new_nbytes = new_ln ? lbuf_s(new_ln)->len : 0;
	int added = new_nbytes - old_nbytes;
	if (added <= 0) {
		free(snap);
		return key;
	}
	int ins_byte = 0;
	while (ins_byte < old_nbytes && snap[ins_byte] == new_ln[ins_byte])
		ins_byte++;
	char *ins_text = new_ln + ins_byte;
	int added_chars = 0;
	for (char *t = ins_text; t < ins_text + added; t += uc_len(t))
		added_chars++;
	for (int r = r1 + 1; r <= r2; r++) {
		char *ln = lbuf_get(xb, r);
		if (!ln)
			continue;
		int n = uc_slen(ln) - 1;	/* index of '\n' = number of content chars */
		int pos;
		if (vcmd == 'I') {
			if (col > n)		/* line too short: skip */
				continue;
			pos = col;
		} else {			/* 'A': insert after right edge */
			pos = MIN(col + 1, n);
		}
		char *p = uc_chr(ln, pos);
		char *nl_p = ln + lbuf_s(ln)->len;	/* pointer to '\n' byte */
		int pre_bytes = p - ln;
		int post_bytes = nl_p - p + 1;		/* bytes from p through '\n' */
		char *new_ln2 = emalloc(pre_bytes + added + post_bytes + 1);
		memcpy(new_ln2, ln, pre_bytes);
		memcpy(new_ln2 + pre_bytes, ins_text, added);
		memcpy(new_ln2 + pre_bytes + added, p, post_bytes);
		new_ln2[pre_bytes + added + post_bytes] = '\0';
		lbuf_edit(xb, new_ln2, r, r + 1, pos, pos + added_chars);
		free(new_ln2);
	}
	free(snap);
	return key;
}

static int vc_block_op(int cmd, int r1, int r2, int c_left, int c_right)
{
	int r;
	if (cmd == 'y' || cmd == 'd' || cmd == 'c') {
		sbuf_smake(yb, 256)
		for (r = r1; r <= r2; r++) {
			char *ln = lbuf_get(xb, r);
			if (!ln)
				continue;
			int n = uc_slen(ln) - 1;	/* number of content chars before '\n' */
			int left = MIN(c_left, n);
			int right = MIN(c_right, n - 1);
			char *beg_p = uc_chr(ln, left);
			char *end_p = right >= left ? uc_chr(beg_p, right - left + 1) : beg_p;
			char *nl_p = ln + lbuf_s(ln)->len;
			if (end_p > nl_p)
				end_p = nl_p;
			if (end_p > beg_p)
				sbuf_mem(yb, beg_p, (int)(end_p - beg_p))
			sbuf_chr(yb, '\n')
		}
		sbuf_null(yb)
		vi_regput(vi_ybuf, yb->s, 0);
		free(yb->s);
	}
	if (cmd == 'd' || cmd == 'c') {
		for (r = r1; r <= r2; r++) {
			char *ln = lbuf_get(xb, r);
			if (!ln)
				continue;
			int n = uc_slen(ln) - 1;
			int left = MIN(c_left, n);
			int right = MIN(c_right, n - 1);
			if (left > right || n == 0)
				continue;
			char *beg_p = uc_chr(ln, left);
			char *end_p = uc_chr(beg_p, right - left + 1);
			char *nl_p = ln + lbuf_s(ln)->len;
			if (end_p > nl_p)
				end_p = nl_p;
			int beg_bytes = (int)(beg_p - ln);
			int rest_bytes = (int)(nl_p - end_p + 1);	/* from end_p through '\n' */
			char *new_ln = emalloc(beg_bytes + rest_bytes + 1);
			memcpy(new_ln, ln, beg_bytes);
			memcpy(new_ln + beg_bytes, end_p, rest_bytes);
			new_ln[beg_bytes + rest_bytes] = '\0';
			lbuf_edit(xb, new_ln, r, r + 1, 0, 0);
			free(new_ln);
		}
		xrow = r1;
		xoff = c_left;
	}
	if (cmd == '~' || cmd == 'u' || cmd == 'U') {
		for (r = r1; r <= r2; r++) {
			char *ln = lbuf_get(xb, r);
			if (!ln)
				continue;
			int n = uc_slen(ln) - 1;
			int left = MIN(c_left, n);
			int right = MIN(c_right, n - 1);
			if (left > right || n == 0)
				continue;
			char *beg_p = uc_chr(ln, left);
			char *end_p = uc_chr(beg_p, right - left + 1);
			char *nl_p = ln + lbuf_s(ln)->len;
			if (end_p > nl_p)
				end_p = nl_p;
			int total = lbuf_s(ln)->len + 2;
			char *new_ln = emalloc(total);
			memcpy(new_ln, ln, total - 1);
			new_ln[total - 1] = '\0';
			char *p = new_ln + (beg_p - ln);
			char *pe = new_ln + (end_p - ln);
			while (p < pe) {
				int ch = (unsigned char)*p;
				if (ch <= 0x7f) {
					if (cmd == 'u')
						*p = (char)tolower(ch);
					else if (cmd == 'U')
						*p = (char)toupper(ch);
					else
						*p = (char)(islower(ch) ? toupper(ch) : tolower(ch));
				}
				p += uc_len(p);
			}
			lbuf_edit(xb, new_ln, r, r + 1, 0, 0);
			free(new_ln);
		}
	}
	if (cmd == '>' || cmd == '<')
		vi_shift(r1, r2, cmd == '>' ? +1 : -1, 1);
	if (cmd == 'c')
		return vc_block_insert('I', r1, r2, c_left);
	return 0;
}

static int vc_visual_op(int cmd)
{
	int r1 = vi_vrow, o1 = vi_voff;
	int r2 = xrow,   o2 = xoff;
	if (r1 > r2 || (r1 == r2 && o1 > o2)) {
		swap(&r1, &r2);
		swap(&o1, &o2);
	}
	if (vi_visual == 'b') {
		int c_left  = MIN(vi_voff, xoff);
		int c_right = MAX(vi_voff, xoff);
		vi_visual = 0;
		vi_mod |= 1;
		return vc_block_op(cmd, r1, r2, c_left, c_right);
	}
	int lnmode = (vi_visual == 'V');
	vi_visual = 0;		/* clear before any operation (vi_change re-enters vi()) */
	vi_mod |= 1;
	if (!lnmode) {
		if (o2 < lbuf_eol(xb, r2, 2))
			o2++;		/* include char under cursor, like vc_motion */
		ren_state *r = (ren_state*)lbuf_get(xb, r1);
		r = r ? ren_position((char*)r) : NULL;
		o1 = r ? MAX(0, MIN(o1, r->n)) : 0;
	} else {
		o1 = 0;
		o2 = lbuf_eol(xb, r2, r1 >= r2);
	}
	int mv_before = lbuf_len(xb);
	int key = 0;
	if (cmd == 'y')
		vi_yank(r1, o1, r2, o2, lnmode);
	else if (cmd == 'd')
		vi_delete(r1, o1, r2, o2, lnmode);
	else if (cmd == 'c')
		key = vi_change(r1, o1, r2, o2, lnmode);
	else if (cmd == '~' || cmd == 'u' || cmd == 'U')
		vi_case(r1, o1, r2, o2, lnmode, cmd);
	else if (cmd == '!')
		vi_pipe(r1, r2);
	else if (cmd == '>' || cmd == '<')
		vi_shift(r1, r2, cmd == '>' ? +1 : -1, 1);
	vi_mod |= r1 != r2 || mv_before != lbuf_len(xb) ? 1 : 2;
	return key;
}

static int vc_motion(int cmd)
{
	int r1 = xrow, r2 = xrow;	/* region rows */
	int o1 = xoff, o2;		/* visual region columns */
	int lnmode = 0;			/* line-based region */
	int mv = vi_prefix();
	term_dec()
	if (mv)
		vi_arg = mv;
	o1 = ren_noeol(lbuf_get(xb, r1), o1);
	o2 = o1;
	if ((mv = vi_motionln(&r2, cmd, vi_arg ? vi_arg : 1)))
		o2 = -1;
	else if (!(mv = vi_motion(cmd, &r2, &o2)))
		return 0;
	if (mv < 0)
		return 0;
	lnmode = o2 < 0;
	if (lnmode) {
		o1 = 0;
		o2 = lbuf_eol(xb, r2, r1 >= r2);
	}
	if (r1 > r2) {
		swap(&r1, &r2);
		swap(&o1, &o2);
	} else if (r1 == r2 && o1 > o2)
		swap(&o1, &o2);
	ren_state *r = (ren_state*)lbuf_get(xb, r1);
	r = r ? ren_position((char*)r) : NULL;
	o1 = r ? MAX(0, MIN(o1, r->n)) : 0;
	if (!lnmode && strchr("fFtTeE%", mv))
		if (o2 < lbuf_eol(xb, r2, 2))
			o2++;
	if (cmd == 'y') {
		vi_yank(r1, o1, r2, o2, lnmode);
		return 0;
	}
	mv = lbuf_len(xb);
	if (cmd == 'd')
		vi_delete(r1, o1, r2, o2, lnmode);
	else if (cmd == 'c')
		return vi_change(r1, o1, r2, o2, lnmode);
	else if (cmd == '~' || cmd == 'u' || cmd == 'U')
		vi_case(r1, o1, r2, o2, lnmode, cmd);
	else if (cmd == '!')
		vi_pipe(r1, r2);
	else if (cmd == '>' || cmd == '<')
		vi_shift(r1, r2, cmd == '>' ? +1 : -1,
			lnmode ? 1 : vi_arg ? vi_arg : 1);
	else if (cmd == TK_CTL('w'))
		vi_shift(r1, r2, -1, INT_MAX / 2);
	vi_mod |= r1 != r2 || mv != lbuf_len(xb) ? 1 : 2;
	return 0;
}

static int vc_insert(int cmd)
{
	char *post, *ln = lbuf_get(xb, xrow);
	int row, cmdo, l1, off, key, postn = 1;
	sbuf_smake(sb, xcols)
	if (cmd == 'I')
		xoff = lbuf_indents(xb, xrow);
	else if (cmd == 'A')
		xoff = lbuf_eol(xb, xrow, 1);
	else if (cmd == 'o') {
		xrow++;
		if (xrow - xtop == xrows)
			vi_drawagain(++xtop);
	}
	xoff = ren_noeol(ln, xoff);
	row = xrow;
	if (cmd == 'a' || cmd == 'A')
		xoff++;
	if (ln && ln[0] == '\n')
		xoff = 0;
	cmdo = cmd == 'o' || cmd == 'O';
	if (cmdo || !ln) {
		if (cmdo && !lbuf_len(xb))
			lbuf_edit(xb, "\n", 0, 0, 0, 0);
		vi_indents(ln, &l1);
		off = l1;
		post = "\n";
	} else {
		off = xoff;
		l1 = rstate->chrs[off] - ln;
		postn = rstate->n - off;
		post = ln + l1;
	}
	term_pos(row - xtop, 0);
	term_room(cmdo);
	sbuf_mem(sb, ln, l1)
	key = led_input(sb, post, postn, row, cmdo << 2, &postn);
	if (postn != l1 || cmdo || !ln) {
		lbuf_edit(xb, sb->s, row, row + !cmdo, off, xoff);
		lmodified = 1;
	} else
		lmodified = 0;
	free(sb->s);
	return key;
}

static int vc_put(int cmd)
{
	int cnt = MAX(1, vi_arg);
	int i, off;
	sbuf *buf = xregs[vi_ybuf];
	char *ln;
	if (!buf)
		vi_drawmsg_mpt("yank buffer empty")
	if (!buf || !buf->s_n)
		return 0;
	sbuf_smake(sb, 1024)
	if (buf->s[buf->s_n-1] == '\n' || strchr(buf->s, '\n')) {
		for (i = 0; i < cnt; i++)
			sbufn_mem(sb, buf->s, buf->s_n)
		if (!lbuf_len(xb))
			lbuf_edit(xb, "\n", 0, 0, 0, 0);
		if (cmd == 'p')
			xrow++;
		lbuf_edit(xb, sb->s, xrow, xrow, 0, 0);
		xoff = lbuf_indents(xb, xrow);
		free(sb->s);
		return 1;
	}
	if (!(ln = lbuf_get(xb, xrow)))
		ln = "\n";
	off = ren_noeol(ln, xoff) + (ln[0] != '\n' && cmd == 'p');
	sbuf_mem(sb, ln, rstate->chrs[off] - ln)
	for (i = 0; i < cnt; i++)
		sbuf_mem(sb, buf->s, buf->s_n)
	sbufn_str(sb, rstate->chrs[off])
	xoff = off + uc_slen(buf->s) * cnt - 1;
	lbuf_edit(xb, sb->s, xrow, xrow + 1, off, xoff);
	free(sb->s);
	return 1;
}

static void vc_join(int spc, int cnt)
{
	int o2 = 0;
	if (lbuf_join(xb, xrow, xrow + cnt, xoff, &o2, spc))
		return;
	xoff = o2;
	vi_mod |= 1;
}

static void vi_scrollforward(int cnt)
{
	xtop = MIN(lbuf_len(xb) - 1, xtop + cnt);
	xrow = MAX(xrow, xtop);
}

static void vi_scrollbackward(int cnt)
{
	xtop = MAX(0, xtop - cnt);
	xrow = MIN(xrow, xtop + xrows - 1);
}

static int vc_replace(void)
{
	int cnt = MAX(1, vi_arg);
	char *cs = led_read(&xkmap, term_read(0));
	char *ln = lbuf_get(xb, xrow);
	int off, i;
	if (!ln || !cs)
		return 0;
	off = ren_noeol(ln, xoff);
	if (off + cnt >= rstate->n)
		return 0;
	sbuf_smake(sb, lbuf_s(ln)->len)
	sbuf_mem(sb, ln, rstate->chrs[off] - ln)
	for (i = 0; i < cnt; i++)
		sbuf_str(sb, cs)
	sbufn_str(sb, rstate->chrs[off+cnt])
	xoff = (off + cnt - 1) * (cs[0] != '\n');
	lbuf_edit(xb, sb->s, xrow, xrow + 1, off, xoff);
	xrow += cnt * (cs[0] == '\n');
	free(sb->s);
	return cs[0] == '\n' ? 1 : 2;
}

static char rep_cmd[sizeof(icmd)];	/* the last command */
static int rep_len;

static void vc_repeat(void)
{
	for (int i = 0; i < MAX(1, vi_arg); i++)
		term_push(rep_cmd, rep_len);
}

static void vc_execute(int cmd)
{
	static int exec_buf = -1;
	int c = term_read(0), i, n = MAX(1, vi_arg);
	sbuf **buf;
	if (TK_INT(c))
		return;
	if (c == cmd && exec_buf >= 0)
		c = exec_buf;
	buf = &xregs[c];
	if (!*buf) {
		vi_drawmsg_mpt("exec buffer empty")
		return;
	}
	exec_buf = c;
	if (c == ':') {
		term_pos(xrows, 0);
		for (i = 0; i < n && *buf; i++)
			ex_exec((*buf)->s);
		vi_mod |= 1;
		return;
	}
	for (i = 0; i < n && *buf; i++)
		term_exec((*buf)->s, (*buf)->s_n, cmd)
}

static void vi_argcmd(int arg, char cmd)
{
	char str[32];
	char *cs = itoa(arg, str);
	*cs = cmd;
	term_push(str, cs - str + 1);
}

#define topfix() \
if (xrow < 0 || xrow >= lbuf_len(xb)) \
	xrow = lbuf_len(xb) ? lbuf_len(xb) - 1 : 0; \
if (xtop > xrow) \
	xtop = xtop - xrows / 2 > xrow ? \
			MAX(0, xrow - xrows / 2) : xrow; \
if (xtop + xrows <= xrow) \
	xtop = xtop + xrows + xrows / 2 <= xrow ? \
			xrow - xrows / 2 : xrow - xrows + 1; \

void vi(int init)
{
	char *ln, *cs;
	int mv, n, k, c;
	xgrec++;
	if (init) {
		topfix()
		vi_col = vi_off2col(xb, xrow, xoff);
		vi_drawagain(xtop);
		term_pos(xrow - xtop, led_pos(lbuf_get(xb, xrow), vi_col));
	}
	while (!xquit) {
		int nrow = xrow;
		int noff = xoff;
		int ooff = noff;
		int otop = xtop;
		int oleft = xleft;
		int orow = xrow;
		icmd_pos = 0;
		vi_mod = 0;
		vi_ybuf = vi_yankbuf();
		vi_arg = vi_prefix();
		term_dec()
		if (vi_lnnum == 1) {
			vi_lnnum = 0;
			vi_lncol = 0;
			vi_mod |= 1;
		}
		if (xmpt == 1) {
			xmpt = 0;
			vi_drawrow(otop + xrows - 1);
		}
		if (led_attsb)
			sbuf_cut(led_attsb, 0)
		if (!vi_ybuf)
			vi_ybuf = vi_yankbuf();
		mv = vi_motion(0, &nrow, &noff);
		if (mv > 0) {
			if (strchr("|jk", mv)) {
				noff = vi_col2off(xb, nrow, vi_col);
			} else {
				noff = noff < 0 ? lbuf_indents(xb, nrow) : noff;
				vi_mod |= 4;
			}
			if ((xrow != nrow || ooff != noff) &&
					strchr("%'`GHML/?{}[]", mv))
				lbuf_mark(xb, '`', xrow, ooff);
			xrow = nrow;
			xoff = noff;
			if (vi_visual)
				vi_mod |= 1;
		} else if (mv == 0) {
			char *cmd;
			term_dec()
			re_motion:
			c = term_read(TK_CTL('l'));
			switch (c) {
			case TK_CTL('b'):
				vi_scrollbackward(MAX(1, vi_arg) * (xrows - 1));
				xoff = lbuf_indents(xb, xrow);
				vi_mod |= 4;
				break;
			case TK_CTL('f'):
				vi_scrollforward(MAX(1, vi_arg) * (xrows - 1));
				xoff = lbuf_indents(xb, xrow);
				vi_mod |= 4;
				break;
			case TK_CTL('e'):
				vi_scrolley = vi_arg ? vi_arg : vi_scrolley;
				vi_scrollforward(MAX(1, vi_scrolley));
				xoff = vi_col2off(xb, xrow, vi_col);
				break;
			case TK_CTL('y'):
				vi_scrolley = vi_arg ? vi_arg : vi_scrolley;
				vi_scrollbackward(MAX(1, vi_scrolley));
				xoff = vi_col2off(xb, xrow, vi_col);
				break;
			case TK_CTL('u'):
				if (xrow == 0)
					break;
				if (vi_arg)
					vi_scrollud = vi_arg;
				n = vi_scrollud ? vi_scrollud : xrows / 2;
				xrow = MAX(0, xrow - n);
				if (xtop > 0)
					xtop = MAX(0, xtop - n);
				xoff = lbuf_indents(xb, xrow);
				vi_mod |= 4;
				break;
			case TK_CTL('d'):
				if (xrow == lbuf_len(xb) - 1)
					break;
				if (vi_arg)
					vi_scrollud = vi_arg;
				n = vi_scrollud ? vi_scrollud : xrows / 2;
				xrow = MIN(MAX(0, lbuf_len(xb) - 1), xrow + n);
				if (xtop < lbuf_len(xb) - xrows)
					xtop = MIN(lbuf_len(xb) - xrows, xtop + n);
				xoff = lbuf_indents(xb, xrow);
				vi_mod |= 4;
				break;
			case TK_CTL('i'): {
				if (!(ln = lbuf_get(xb, xrow)))
					break;
				ln += xoff;
				char buf[strlen(ln)+4];
				strcpy(buf, ":e ");
				strcpy(buf+3, ln);
				term_push(buf, strlen(ln)+3);
				break; }
			case TK_CTL('n'):
				vi_cndir = vi_arg ? -vi_cndir : vi_cndir;
				vi_arg = ex_buf - bufs + vi_cndir;
			case TK_CTL('_'):	/* this is also ^7 on some systems */
				if (vi_arg > 0)
					goto switchbuf;
				ex_exec("left0:b:mpt0");
				term_chr('\n');
				vi_arg = vi_digit();
				if (vi_arg > -1 && vi_arg < xbufcur) {
					switchbuf:
					bufs_switchwft(vi_arg < xbufcur ? vi_arg : 0)
					vc_status(0);
				}
				vi_mod |= 1;
				break;
			case 'u':
				if (vi_visual) {
					vc_visual_op('u');
					break;
				}
				undo:
				if (vi_arg >= 0 && !lbuf_undo(xb, &xrow, &xoff)) {
					vi_mod |= 1;
					vi_arg--;
					goto undo;
				} else if (!vi_arg)
					vi_drawmsg_mpt("undo failed")
				break;
			case TK_CTL('r'):
				redo:
				if (vi_arg >= 0 && !lbuf_redo(xb, &xrow, &xoff)) {
					vi_mod |= 1;
					vi_arg--;
					goto redo;
				} else if (!vi_arg)
					vi_drawmsg_mpt("redo failed")
				break;
			case TK_CTL('g'):
				vi_tsm = 0;
				status:
				if (vi_arg) {
					vi_status = vi_arg > 1 ? 0 : term_resized;
					xrows += vi_status ? -1 : 1;
				}
				vc_status(vi_tsm);
				break;
			case TK_CTL('^'):
				bufs_switchwft(ex_pbuf - bufs)
				vc_status(0);
				vi_mod |= 1;
				break;
			case TK_CTL('k'):;
				static struct lbuf *writexb;
				if ((cs = ex_exec("w")) && writexb && xb == writexb)
					cs = ex_exec("mpt0:w!");
				writexb = cs ? xb : NULL;
				vi_mod |= 1;
				break;
			case '#':
				if (vi_lnnum & vi_arg)
					vi_lnnum = vi_lnnum & ~vi_arg;
				else
					vi_lnnum = vi_arg ? vi_lnnum | vi_arg : !vi_lnnum;
				vi_lncol = 0;
				vi_mod |= 1;
				break;
			case 'v':
				if (vi_visual == 'v') {
					vi_visual = 0;
				} else {
					vi_visual = 'v';
					vi_vrow = xrow;
					vi_voff = xoff;
				}
				vi_mod |= 1;
				break;
			case 'U':
				if (vi_visual) {
					vc_visual_op('U');
					break;
				}
				continue;
			case 'V':
				if (vi_visual == 'V') {
					vi_visual = 0;
				} else {
					vi_visual = 'V';
					vi_vrow = xrow;
					vi_voff = xoff;
				}
				vi_mod |= 1;
				break;
			case TK_CTL('v'):
				if (vi_visual == 'b')
					vi_visual = 0;
				else {
					vi_visual = 'b';
					vi_vrow = xrow;
					vi_voff = xoff;
				}
				vi_mod |= 1;
				break;
			case TK_CTL('c'):
				if (!vi_arg)
					vi_arg = (vi_wsel % 5) + !!*vi_word;
				if (vi_arg && vi_arg <= 5) {
					vi_wsel = vi_arg;
					vi_word = _vi_word + vi_arg;
				} else
					vi_word = _vi_word + (!*vi_word * vi_wsel);
				vi_rshift = 0;
				vi_mod |= 1;
				break;
			case ':':
				ln = vi_enprompt(":", NULL, &k, &n);
				do_excmd:
				if (k && ln[n])
					ex_command(ln + n)
				vi_mod |= 1;
				if (!xmpt)
					vi_drawmsg(ln);
				free(ln);
				if (xquit) {
					xmpt = xmpt ? xmpt : (xgrec > 1);
					continue;
				} else if (!xmpt)
					xmpt = 1;
				break;
			case 'c':
				if (vi_visual) {
					k = vc_visual_op('c');
					goto ins;
				}
				/* fall through */
			case 'd':
				if (vi_visual) {
					vc_visual_op('d');
					break;
				}
				k = term_read(0);
				if (k == 'i') {
					k = term_read(0);
					switch(k) {
					case ')':
						term_push("F(ldt)", 6);
						break;
					case '(':
						term_push("f(ldt)", 6);
						break;
					case '"':
						term_push("f\"ldt\"", 6);
						break;
					}
					if (c == 'c')
						term_back('i');
					break;
				}
				term_dec()
			case 'y':
			case '!':
			case '>':
			case '<':
			case TK_CTL('w'):
				if (vi_visual && c != TK_CTL('w')) {
					vc_visual_op(c);
					break;
				}
				k = vc_motion(c);
				if (c == 'c')
					goto ins;
				break;
			case 'I':
			case 'i':
			case 'a':
			case 'A':
			case 'o':
			case 'O':
				if (vi_visual == 'b' && (c == 'I' || c == 'A')) {
					int r1b = MIN(vi_vrow, xrow), r2b = MAX(vi_vrow, xrow);
					int c_left = MIN(vi_voff, xoff), c_right = MAX(vi_voff, xoff);
					vi_visual = 0;
					k = vc_block_insert(c, r1b, r2b, c == 'I' ? c_left : c_right);
					goto ins;
				}
				k = vc_insert(c);
				ins:
				vi_mod |= !xpac && xrow == orow ? 8 : 1;
				if (k == 127) {
					if (xrow && !(xoff > 0 && lbuf_eol(xb, xrow, 1))) {
						xrow--;
						vc_join(0, 2);
					} else if (xoff)
						vi_delete(xrow, xoff - 1, xrow, xoff, 0);
					term_back(xoff != lbuf_eol(xb, xrow, 1) ? 'i' : 'a');
					break;
				}
				switch (k) {
				case 'A':	/* ↑ */
					term_back(!lmodified ? c : 'i');
					if (lmodified)
						vi_col = vi_off2col(xb, xrow, xoff);
					xrow--;
					xrow = xrow < 0 ? 0 : xrow;
					xoff = vi_col2off(xb, xrow, vi_col);
					lmodified = 0;
					goto _break;
				case 'B':	/* ↓ */
					term_back(!lmodified ? c : 'i');
					if (lmodified)
						vi_col = vi_off2col(xb, xrow, xoff);
					xrow++;
					xoff = vi_col2off(xb, xrow, vi_col);
					lmodified = 0;
					goto _break;
				case 'D':	/* ← */
					term_back('i');
					xoff--;
					xoff = xoff < 0 ? 0 : xoff;
					vi_col = vi_off2col(xb, xrow, xoff);
					goto _break;
				case 'C':	/* → */
					term_back(*uc_chr(lbuf_get(xb, xrow), xoff+2) ? 'i' : 'A');
					xoff++;
					if (*uc_chr(lbuf_get(xb, xrow), xoff))
						vi_col = vi_off2col(xb, xrow, xoff);
					goto _break;
				}
				if (c != 'A' && c != 'C')
					xoff--;
				break;
				_break:
				vi_mod = 0;
				break;
			case 'J':
				vc_join(1, vi_arg <= 1 ? 2 : vi_arg);
				break;
			case 'K': {
				preserve(int, xled, xled = 0;)
				do {
					ex_exec(";+1c\n\x1b:-1");
				} while (vi_arg--);
				restore(xled)
				vi_mod |= 1;
				break; }
			case TK_CTL('z'):
			case TK_CTL('l'):
				if (c == TK_CTL('z')) {
					term_pos(xrows, 0);
					term_suspend();
				} else {
					term_done();
					term_init();
				}
				vi_mod |= 1;
				break;
			case 'm':
				lbuf_mark(xb, term_read(0), xrow, xoff);
				break;
			case 'p':
			case 'P':
				vi_mod |= vc_put(c);
				break;
			case 'z':
				k = term_read(0);
				switch (k) {
				case '\n':
					xtop = xrow;
					break;
				case '.':
					xtop = MAX(0, xrow - xrows / 2);
					break;
				case '-':
					xtop = MAX(0, xrow - xrows + 1);
					break;
				case 'l':
				case 'r':
				case 'L':
				case 'R':
					xtd = isupper(k)+1;
					xtd = tolower(k) == 'r' ? -xtd : xtd;
					rstates[0].s = NULL;
					rstates[1].s = NULL;
					break;
				case 'e':
				case 'f':
					xkmap = k == 'e' ? 0 : xkmap_alt;
					break;
				case '1':
				case '2':
					xkmap_alt = k - '0';
					break;
				}
				vi_mod |= 1;
				break;
			case 'g':
				k = term_read(0);
				if (k == 'g')
					term_push("1G", 2);
				else if (k == 'a') {
					vi_tsm = 1;
					goto status;
				} else if (k == 'w') {
					preserve(int, xled, xled = 0;)
					preserve(int, xgrp, xgrp = 2;)
					n = vi_arg ? vi_arg : 80;
					while (1) {
						xoff = vi_col2off(xb, xrow, n);
						vi_col = vi_off2col(xb, xrow, xoff+1);
						if (vi_col <= n)
							break;
						if (ex_exec("f>[^ \t]*[ \t]+(?\\:.$|(.)):??;c\n\x1b"))
							break;
					}
					restore(xled)
					restore(xgrp)
					vi_mod |= !texec;
				} else if (k == 'q') {
					preserve(int, xled, xled = 0;)
					char cmd[64] = "g/./& ";
					strcpy(itoa(vi_arg, cmd+5), "gw");
					ex_command(cmd)
					restore(xled)
					vi_mod |= 1;
				} else if (k == '~' || k == 'u' || k == 'U') {
					vc_motion(k);
					goto rep;
				} else if (k == '.') {
					vi_mod |= 2;
					while (vi_arg) {
						term_push("j", 1);
						term_push(rep_cmd, rep_len);
						if (strchr("iIoOaAsScC", rep_cmd[0])) {
							term_push("0", 1);
							if (noff)
								vi_argcmd(noff, 'l');
						}
						vi_arg--;
					}
				} else if (k == 'W') {
					vi_nlword = !vi_nlword;
				} else if (k == 'o') {
					ex_command("%s/\x0d//g:%s/[ \t]+$//g")
					vi_mod |= 1;
				} else if (k == 'I' || k == 'i') {
					vi_mod |= 2;
					char restr[100] = "%s/^\t/";
					vi_arg = MIN(vi_arg ? vi_arg : xts, 80);
					if (k == 'I') {
						cmd = restr+6;
						while (vi_arg--)
							*cmd++ = ' ';
						strcpy(cmd, "/g");
					} else {
						strcpy(restr, "%s/^ {");
						strcpy(itoa(vi_arg, restr+6), "}/\t/g");
					}
					ln = vi_enprompt(":", restr, &k, &n);
					goto do_excmd;
				} else if (k == 'b' || k == 'v') {
					term_push(k == 'v' ? ":\x01" : ":\x02", 2); /* ^a : ^b */
				} else if (k == ';') {
					ln = vi_enprompt(":", "!", &k, &n);
				} else if (k == '/') {
					cs = vi_curword(xb, xrow, xoff, vi_arg, 1);
					char buf[cs ? strlen(cs)+30 : 30];
					strcpy(buf, "re ");
					if (cs)
						strcat(buf, cs);
					free(cs);
					ln = vi_enprompt(":", buf, &k, &n);
					goto do_excmd;
				} else if (k == 't') {
					vi_drawmsg("arg2:(0|#)");
					cs = vi_curword(xb, xrow, xoff, vi_prefix(), 1);
					char buf[cs ? strlen(cs)+30 : 30];
					strcpy(buf, ".,.+");
					char *buf1 = itoa(vi_arg, buf+4);
					strcat(buf1, "s/");
					if (cs) {
						strcat(buf1, cs);
						strcat(buf1, "/");
						free(cs);
					}
					ln = vi_enprompt(":", buf, &k, &n);
					goto do_excmd;
				} else if (k == 'r') {
					cs = vi_curword(xb, xrow, xoff, vi_arg, 1);
					char buf[cs ? strlen(cs)+30 : 30];
					strcpy(buf, "%s/");
					if (cs) {
						strcat(buf, cs);
						strcat(buf, "/");
						free(cs);
					}
					ln = vi_enprompt(":", buf, &k, &n);
					goto do_excmd;
				} else if (k == 'V') {
					vi_hidch = !vi_hidch;
					vi_mod |= 1;
				} else {
					term_dec()
				}
				break;
			case 'x':
				term_push("d ", 2);
				goto motion;
			case 'X':
				term_push("d", 2);
				goto motion;
			case 'D':
				term_push("d$", 2);
				goto motion;
			case 'Y':
				term_push("yy", 2);
				goto motion;
			case '~':
				if (vi_visual) {
					vc_visual_op('~');
					break;
				}
				term_push("g~ ", 3);
				goto motion;
			case 'C':
				term_push("c$", 2);
				goto motion;
			case 's':
				term_push("c ", 2);
				goto motion;
			case 'S':
				term_push("cc", 2);
				motion:
				icmd_pos--;
				goto re_motion;
			case 'r':
				vi_mod |= vc_replace();
				break;
			case 'R':
				ex_exec("left0:reg");
				break;
			case 'Q':
				term_pos(xrow - xtop, 0);
				xleft = vi_arg ? xleft : 0;
				led_modeswap();
				vi_mod |= 1;
				if (xquit)
					continue;
				break;
			case 'Z':
				k = term_read(0);
				if (k == 'Z') {
					ex_exec("x");
					continue;
				}
				xquit = texec == '&' ? -1 : 1;
				if (k == 'z')
					term_push("\n", 1);
				else if (xgrec == 1) {
					term_clean();
					xgrec = 0;
				}
				continue;
			case '.':
				vc_repeat();
				break;
			case '@':
			case '&':
				vc_execute(c);
				break;
			case '\\':
				if (!vi_arg)
					ex_exec("b-2");
				else if (xb != tempbufs[1].lb)
					ex_exec("b-2:%d:fd:b-2");
				else
					ex_exec("%d:fd");
				vc_status(0);
				vi_mod |= 1;
				break;
			case TK_ESC:
				if (vi_visual) {
					vi_visual = 0;
					vi_mod |= 1;
					break;
				}
				continue;
			default:
				continue;
			}
			if (strchr("!<>AIJKOPRacdiopry", c)) {
				rep:
				memcpy(rep_cmd, icmd, icmd_pos);
				rep_len = icmd_pos;
			}
		}
		topfix()
		ln = lbuf_get(xb, xrow);
		xoff = ren_noeol(ln, xoff);
		if (ln && !rstate->wid[xoff]) {
			for (n = xoff, k = n; k < rstate->n && !rstate->wid[k];) {
				if (!k)
					n = ooff+1;
				k += n > ooff ? 1 : -1;
			}
			if (k < rstate->n)
				xoff = k;
		}
		if (vi_mod)
			vi_col = vi_off2col(xb, xrow, xoff);
		if (vi_col >= xleft + xcols || vi_col < xleft)
			xleft = vi_col < xcols ? 0 : vi_col - xcols / 2;
		n = led_pos(ln, ren_cursor(ln, vi_col));
		if (xmpt > 1) {
			if (!xpln)
				term_chr('\n');
			vi_drawmsg("[any key to continue] ");
			term_read(0);
			xmpt = 0;
			vi_mod |= 1;
		}
		xpln = 0;
		if (xhlw) {
			static char *word;
			if ((cs = vi_curword(xb, xrow, xoff, xhlw, 0))) {
				if (!word || strcmp(word, cs)) {
					syn_reloadft(syn_addhl(cs, 1), 0);
					vi_mod |= 1;
				}
				free(word);
				word = cs;
			}
		}
		if (xhlp && (k = syn_findhl(3)) >= 0) {
			int row = xrow, off = xoff, row1, off1;
			led_att la;
			if (!led_attsb)
				sbuf_make(led_attsb, sizeof(la) * 2)
			if (!lbuf_pair(xb, &row, &off)) {
				row1 = row; off1 = off;
				if (!lbuf_pair(xb, &row, &off)) {
					la.s = ln;
					la.off = off;
					la.att = hls[k].att[0];
					sbuf_mem(led_attsb, &la, (int)sizeof(la))
					la.s = lbuf_get(xb, row1);
					la.off = off1;
					sbuf_mem(led_attsb, &la, (int)sizeof(la))
					vi_mod |= row1 == row && orow == xrow ? 2 : 1;
				}
			}
		}
		term_record = 1;
		if (vi_mod & 1 || xleft != oleft
				|| (vi_lnnum && orow != xrow && !(vi_lnnum == 2))
				|| (*vi_word && orow != xrow))
			vi_drawagain(xtop);
		else if (*vi_word && (ooff != xoff || vi_mod & 2)
				&& xrow+1 < xtop + xrows)
			vi_drawrow(xrow+1);
		else if (xtop != otop)
			vi_drawupdate(otop - xtop);
		if (xhll) {
			syn_blockhl = -1;
			if (xrow != orow && orow >= xtop && orow < xtop + xrows)
				if (!(vi_mod & 1))
					vi_drawrow(orow);
			syn_blockhl = -1;
			syn_reloadft(syn_addhl("^.+", 2), 0);
			vi_drawrow(xrow);
			syn_reloadft(syn_addhl(NULL, 2), 0);
		} else if (vi_mod & 2 && !(vi_mod & 1)) {
			syn_blockhl = -1;
			vi_drawrow(xrow);
		}
		if (vi_status && xmpt < 1) {
			xrows -= term_resized != vi_status;
			vi_status = term_resized;
			vc_status(vi_tsm);
			if (xmpt > 0)
				xmpt = 0;
		}
		term_pos(xrow - xtop, n + vi_lncol);
		term_commit();
		xb->useq += xseq;
	}
	if (--xgrec == 0) {
		term_pos(xrows - !vi_status, 0);
		if (xmpt > 0 && !xpln)
			term_chr('\n');
		else
			term_kill();
	}
}

static void sighandler(int signo)
{
	term_winch++;
}

static void setup_signals(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	sigaction(SIGWINCH, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
}

int main(int argc, char *argv[])
{
	int i, j, cmdnum = 0;
	char *ex_cmds[argc - 1];
	setup_signals();
	dir_init();
	syn_init();
	temp_open(0, "/hist/", _ft);
	temp_open(1, "/fm/", fm_ft);
	temp_open(2, "/sc/", _ft);
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (argv[i][1] == '-' && !argv[i][2]) {
			i++;
			break;
		} else if (!argv[i][1])
			stdin_fd = MAX(0, open(ctermid(NULL), O_RDONLY));
		for (j = 1; argv[i][j]; j++) {
			if (argv[i][j] == 's')
				xvis |= 1|2;
			else if (argv[i][j] == 'e')
				xvis |= 2;
			else if (argv[i][j] == 'm')
				xvis |= 4;
			else if (argv[i][j] == 'a')
				xvis |= 8;
			else if (argv[i][j] == 'R')
				readonly = 1;
			else if (argv[i][j] == 'v')
				xvis = 0;
			else if (argv[i][j] == 'c') {
				if (argv[i][j+1]) {
					ex_cmds[cmdnum++] = argv[i] + j + 1;
					break;
				} else if (i + 1 < argc) {
					ex_cmds[cmdnum++] = argv[++i];
					break;
				} else {
					fprintf(stderr, "Missing argument for -c\n");
					return EXIT_FAILURE;
				}
			} else {
				fprintf(stderr, "Unknown option: -%c\n", argv[i][j]);
				fprintf(stderr, "Nextvi-4.2 Usage: %s [-acemRsv] [file ...]\n", argv[0]);
				return EXIT_FAILURE;
			}
		}
	}
	ibuf = emalloc(ibuf_sz);
	if (!(xvis & 1))
		term_init();
	if (xvis & 8)
		term_scrh;
	ex_init(argv + i, argc - i, ex_cmds, cmdnum);
	if (xvis & 2)
		ex();
	else
		vi(1);
	term_done();
	if (xvis & 8)
		term_scrl;
	return abs(xquit) - 1;
}
