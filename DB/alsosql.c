/*
  *
  * This file implements basic SQL commands of AlchemyDatabase (single row ops)
  *  and calls the range-query and join ops
  *

AGPL License

Copyright (c) 2011 Russell Sullivan <jaksprats AT gmail DOT com>
ALL RIGHTS RESERVED 

   This file is part of ALCHEMY_DATABASE

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <assert.h>

#include "xdb_hooks.h"

#include "redis.h"
#include "zmalloc.h"

#include "debug.h"
#include "find.h"
#include "prep_stmt.h"
#ifdef EMBEDDED_VERSION
#include "embed.h"
#endif
#include "lru.h"
#include "lfu.h"
#include "rpipe.h"
#include "desc.h"
#include "bt.h"
#include "filter.h"
#include "index.h"
#include "range.h"
#include "cr8tblas.h"
#include "wc.h"
#include "parser.h"
#include "colparse.h"
#include "find.h"
#include "query.h"
#include "aobj.h"
#include "common.h"
#include "alsosql.h"

extern r_tbl_t *Tbl;   extern int Num_tbls;
extern r_ind_t *Index; extern int Num_indx;

// GLOBALS
bool   GlobalNeedCn = 0;

// CONSTANT GLOBALS
char *EMPTY_STRING = "";
char  OUTPUT_DELIM = ',';

/* enum OP              {NONE,   EQ,     NE,     GT,     GE,     LT,    LE}; */
char *OP_Desc   [NOP] = {"",    "=",    "!=",   ">",    ">=",   "<",   "<=", 
                            "RangeQuery", "IN", "LuaFunction"};
uchar OP_len    [NOP] = {0,      1,      2,      1,      2,      1,     2,
                            -1,           -1,    -1};
aobj_cmp *OP_CMP[NOP] = {NULL, aobjEQ, aobjNE, aobjLT, aobjLE, aobjGT, aobjGE,
                            NULL,         NULL,  NULL};
/* NOTE ranges (<,<=,>,>=) comparison functions are opposite of intuition */

char *RangeType[5] = {"ERROR", "SINGLE_PK", "RANGE", "IN", "SINGLE_FK"};

/* PROTOTYPES */
static int updateAction(cli *c, char *u_vallist, aobj *u_apk, int u_tmatch);

/* INSERT INSERT INSERT INSERT INSERT INSERT INSERT INSERT INSERT INSERT */
static void addRowSizeReply(cli *c, int tmatch, bt *btr, int len) {
    char buf[128];
    ull  index_size = get_sum_all_index_size_for_table(tmatch);
    snprintf(buf, 127,
             "INFO: BYTES: [ROW: %d BT-TOTAL: %ld [BT-DATA: %ld] INDEX: %lld]",
             len, btr->msize, btr->dsize, index_size);
    buf[127] = '\0';
    robj *r  = _createStringObject(buf); addReplyBulk(c, r); decrRefCount(r);
}
#define DEBUG_INSERT_DEBUG                                                   \
  printf("DO update: %d (%s) tmatch: %d apk: ",                              \
          update, c->argv[update]->ptr, tmatch); dumpAobj(printf, &apk);
#define DEBUG_REPLY_LIST \
  { listNode  *ln; listIter  *li = listGetIterator(c->reply, AL_START_HEAD); \
    while((ln = listNext(li))) {                                             \
        robj *r = ln->value; printf("REPLY: %s\n", r->ptr);                  \
    } listReleaseIterator(li); }
#define DEBUG_UPDATE_SNGL                                                    \
  printf("SINGLE ROW UPDATE: exists: %d miss: %d upx: %d\n",                 \
         exists, dwm.miss, upx);

uchar insertCommit(cli  *c,      sds     uset,   sds     vals,
                   int   ncols,  int     tmatch, int     matches,
                   int   inds[], int     pcols,  list   *cmatchl,
                   bool  repl,   uint32  upd,    uint32 *tsize,
                   bool  parse,  sds    *key) {
    CMATCHS_FROM_CMATCHL twoint cofsts[ncols];
    for (int i = 0; i < ncols; i++) cofsts[i].i = cofsts[i].j = -1;
    aobj     apk; initAobj(&apk);
    bool     ret    = INS_ERR;    /* presume failure */
    void    *nrow   = NULL;       /* B4 GOTO */
    sds      pk     = NULL;
    int      pklen  = 0;                            // NEEDED? use sdslen(pk)
    r_tbl_t *rt     = &Tbl[tmatch];
    bt      *btr    = getBtr(tmatch);
    int      lncols = ncols;
    if (rt->lrud) lncols--; if (rt->lfu) lncols--; // INSERT CANT have LRU/LFU
    bool     ai     = 0;    // AUTO-INCREMENT
    char    *mvals  = parseRowVals(vals, &pk, &pklen, ncols, cofsts, tmatch,
                                   pcols, ics, lncols, &ai); // ERR NOW GOTO
    if (!mvals) { addReply(c, shared.insertcolumn);                goto insc_e;}
    if (parse) { // used in cluster-mode to get sk's value
        int skl = cofsts[rt->sk].j - cofsts[rt->sk].i;
        sds sk  = rt->sk ? sdsnewlen(mvals + cofsts[rt->sk].i, skl) : pk;
        *key    = sdscatprintf(sdsempty(), "%s=%s.%s", 
                               sk, rt->name, rt->col[rt->sk].name);
        return ret;
    }
    int      pktyp  = rt->col[0].type;
    apk.type        = apk.enc = pktyp; apk.empty = 0;
    if        C_IS_I(pktyp) {
        long l      = atol(pk);                            /* OK: DELIM: \0 */
        if (l >= TWO_POW_32) { addReply(c, shared.uint_pkbig);     goto insc_e;}
        apk.i       = (int)l;
    } else if C_IS_L(pktyp) apk.l = strtoul(pk, NULL, 10); /* OK: DELIM: \0 */
      else if C_IS_X(pktyp) {
          bool r = parseU128(pk, &apk.x); 
          if (!r) { addReply(c, shared.u128_parse);                goto insc_e;}
    } else if C_IS_F(pktyp) apk.f = atof(pk);              /* OK: DELIM: \0 */
      else if C_IS_S(pktyp) {
        apk.s       = pk; apk.len = pklen; apk.freeme = 0; /* pk freed below */
    } else assert(!"insertCommit ERROR");
    int    len      = 0;
    dwm_t  dwm      = btFindD(btr, &apk);
    void  *orow     = dwm.k;
    bool   gost     = IS_GHOST(btr, orow);
    bool   exists   = (orow || dwm.miss) && !gost;
    bool   isinsert = !upd && !repl;
    if (rt->dirty) { // NOTE: DirtyTable's have prohibited actions
        if (repl) { // REPLACE & indexed-table
            addReply(c, shared.replace_dirty);                     goto insc_e;
        }
        if (isinsert && !ai) { //INSERT on DIRTY w/ PK declation PROHIBITED
            addReply(c, shared.insert_dirty_pkdecl);               goto insc_e;
        }
    }
    if        (exists && isinsert) {
         addReply(c, shared.insert_ovrwrt);                        goto insc_e;
    } else if (exists && upd) {                            //DEBUG_INSERT_DEBUG
        len = updateAction(c, uset, &apk, tmatch);
        if (len == -1)                                             goto insc_e;
        ret = INS_UP;             /* negate presumed failure */
    } else { // UPDATE on GHOST/new, REPLACE, INSERT
        nrow = createRow(c, &apk, btr, tmatch, ncols, mvals, cofsts);
        if (!nrow) /* e.g. (UINT_COL > 4GB) error */               goto insc_e;
        if (!runFailableInsertIndexes(c, btr, &apk, nrow,
                                      matches, inds))              goto insc_e;
        lua_getglobal (server.lua, "run_ALL_AQ"); // set ALL LuaTables
        DXDB_lua_pcall(server.lua, 0, 0, 0);
        if (rt->nltrgr) {
            runLuaTriggerInsertIndexes(c, btr, &apk, nrow, matches, inds);
        }
        if (repl && orow) { /* Delete repld row's Indexes - same PK */
            runDeleteIndexes(btr, &apk, orow, matches, inds, 0);
        }
        //printf("repl: %d orow: %p upd: %d miss: %d exists: %d key: ",
        //     repl, orow, upd, dwm.miss, exists); dumpAobj(printf, &apk);
        len = repl ? btReplace(btr, &apk, nrow) : btAdd(btr, &apk, nrow);
        UPDATE_AUTO_INC(pktyp, &apk)
        ret = INS_INS;            /* negate presumed failure */
    }
    if (tsize) *tsize = *tsize + len;
    server.dirty++;

insc_e:
    if (!ret) {
        lua_getglobal (server.lua, "reset_AQ");
        DXDB_lua_pcall(server.lua, 0, 0, 0);
    }
    if (nrow && NORM_BT(btr)) free(nrow);                /* FREED 023 */
    if (pk)                   free(pk);                  /* FREED 021 */
    releaseAobj(&apk);
    return ret;
}
#define DEBUG_INSERT_ACTION_1 \
  for (int i = 0; i < c->argc; i++) \
    printf("INSERT: cargv[%d]: %s\n", i, c->argv[i]->ptr);

#define AEQ(a,b) !strcasecmp(c->argv[a]->ptr, b)

static bool checkRepeatHashCnames(cli *c, int tmatch) {
    r_tbl_t *rt = &Tbl[tmatch];
    if (rt->tcols < 2) return 1;
    for (uint32 i = 0; i < rt->tcols; i++) {
        for (uint32 j = 0; j < rt->tcols; j++) { if (i == j) continue;
            if (!strcmp(rt->tcnames[i], rt->tcnames[j])) {
                addReply(c, shared.repeat_hash_cnames); return 0;
    }}}
    return 1;
}
static void resetTCNames(int tmatch) {
    r_tbl_t *rt  = &Tbl[tmatch]; if (!rt->tcols) return;
    for (uint32 i = 0; i < rt->tcols; i++) sdsfree(rt->tcnames[i]); //FREED 107
    free(rt->tcnames);                                              //FREED 106
    rt->tcnames = NULL; rt->tcols = 0; rt->ctcol = 0;
}
static bool insertColDeclParse(cli *c,       robj **argv, int tmatch, int *valc,
                              list *cmatchl, list *ls,    int *pcols) {
    r_tbl_t *rt   = &Tbl[tmatch];
    bool     ok   = 0;
    sds      cols = argv[*valc]->ptr;
    if (cols[0] == '(' && cols[sdslen(cols) - 1] == ')' ) { /* COL DECL */
        STACK_STRDUP(clist, (cols + 1), (sdslen(cols) - 2));
        if (!parseCSLSelect(c, clist, 0, 1, tmatch, cmatchl,
                                       ls, pcols, NULL))   return 0;
        if (rt->tcols && !checkRepeatHashCnames(c, tmatch)) return 0;
        if (*pcols) {
            if (initL_LRUCS(tmatch, cmatchl)) { /* LRU in ColDecl */
                addReply(c, shared.insert_lru);             return 0;
            }
            if (initL_LFUCS(tmatch, cmatchl)) { /* LFU in ColDecl */
                addReply(c, shared.insert_lfu);             return 0;
            }
            int cm0 = (int)(long)cmatchl->head->value;
            if (OTHER_BT(getBtr(tmatch)) && *pcols != 2 && !cm0) {
                addReply(c, shared.part_insert_other);      return 0;
            }
            INCR(*valc); if (!strcasecmp(argv[*valc]->ptr, "VALUES")) ok = 1;
        }
    }
    if (!ok) addReply(c, shared.insertsyntax_no_values);
    return ok;
}
void insertParse(cli *c, robj **argv, bool repl, int tmatch,
                 bool parse, sds *key) {
    resetTCNames(tmatch); MATCH_INDICES(tmatch)
    r_tbl_t *rt      = &Tbl[tmatch];
    int      ncols   = rt->col_count; /* NOTE: need space for LRU */
    CREATE_CS_LS_LIST(1)
    int      pcols   = 0;
    int      valc    = 3;
    if (strcasecmp(argv[valc]->ptr, "VALUES")) {//TODO break block into func
        if (!insertColDeclParse(c, argv, tmatch, &valc, cmatchl, ls, &pcols))
                                                   goto insprserr;
    }
    bool print = 0; uint32 upd = 0; int largc = c->argc;
    if (largc > 5) {
        if (AEQ((largc - 1), "RETURN SIZE")) {
            largc--; print = 1; // DO NOT REPLICATE "RETURN SIZE"
            sdsfree(c->argv[largc]->ptr); c->argv[largc]->ptr = sdsempty();
        }
        if (largc > 6) {
            if (AEQ((largc - 2), "ON DUPLICATE KEY UPDATE")) {
                upd = (uint32)largc - 1; largc -= 2;
        }}
    }
    if (upd && repl) {
        addReply(c, shared.insert_replace_update); goto insprserr;
    }
    uchar ret    = INS_ERR; uint32 tsize = 0;
    ncols       += rt->tcols; // ADD in HASHABILITY columns
    sds   uset   = upd ? argv[upd]->ptr : NULL;
    for (int i = valc + 1; i < largc; i++) {
        ret = insertCommit(c, uset, argv[i]->ptr, ncols, tmatch, matches, inds,
                           pcols, cmatchl, repl, upd, print ? &tsize : NULL,
                           parse, key);
        if (ret == INS_ERR)                        goto insprserr;
    }
    if (print) addRowSizeReply(c, tmatch, getBtr(tmatch), tsize);
    else       addReply(c, shared.ok);

insprserr:
    RELEASE_CS_LS_LIST
}
static void insertAction(cli *c, bool repl) {           //DEBUG_INSERT_ACTION_1
   if (strcasecmp(c->argv[1]->ptr, "INTO")) {
        addReply(c, shared.insertsyntax_no_into); return;
    }
    int      len   = sdslen(c->argv[2]->ptr);
    char    *tname = rem_backticks(c->argv[2]->ptr, &len); /* Mysql compliant */
    TABLE_CHECK_OR_REPLY(tname,)
    insertParse(c, c->argv, repl, tmatch, 0, NULL);
}
/* NOTE: INSERT HAS 4 SYNTAXES
     1: INSERT INTO tbl VALUES "(,,,,)"
     2: INSERT INTO tbl VALUES "(,,,,)" "(,,,,)" "(,,,,)"
     3: INSERT INTO tbl VALUES "(,,,,)" "ON DUPLICATE KEY UPDATE" update_stmt
     4: INSERT INTO tbl VALUES "(,,,,)" "RETURN SIZE" */
void insertCommand (cli *c) { insertAction(c, 0); }
void replaceCommand(cli *c) { insertAction(c, 1); }

//TODO move to orderby.c
void init_wob(wob_t *wb) {
    bzero(wb, sizeof(wob_t)); wb->lim = wb->ofst = -1;
}
void destroy_wob(wob_t *wb) {
    for (uint32 i = 0; i < wb->nob; i++) releaseIC(&wb->obc[i]);
    if (wb->ovar) sdsfree(wb->ovar);
}

// SERIALISE_WB SERIALISE_WB SERIALISE_WB SERIALISE_WB SERIALISE_WB
#define WB_CTA_SIZE 9 /* (sizeof(int) * 2 + sizeof(bool)) */
#define WB_LIM_OFST_SIZE 16 /* sizeof(long) *2 */

int getSizeWB(wob_t *wb) {
    if (!wb->nob) return sizeof(int); // nob
    //        nob      +      [obc,obt,asc]      + [lim + ofst]
    return sizeof(int) + (WB_CTA_SIZE * wb->nob) + WB_LIM_OFST_SIZE;
}
#define SERIAL_WB_SIZE MAX_ORDER_BY_COLS * WB_CTA_SIZE + WB_LIM_OFST_SIZE
static uchar SerialiseWB_Buf[SERIAL_WB_SIZE];
uchar *serialiseWB(wob_t *wb) {
    uchar *x = (uchar *)&SerialiseWB_Buf;
    memcpy(x, &wb->nob, sizeof(int));         x += sizeof(int);
    if (!wb->nob) return (uchar *)&SerialiseWB_Buf;
    for (uint32 i = 0; i < wb->nob; i++) {
        memcpy(x, &wb->obt[i], sizeof(int));  x += sizeof(int);
        memcpy(x, &wb->obc[i], sizeof(int));  x += sizeof(int);
        memcpy(x, &wb->asc[i], sizeof(bool)); x += sizeof(bool);
    }
    memcpy(x, &wb->lim,  sizeof(long));       x += sizeof(long);
    memcpy(x, &wb->ofst, sizeof(long));       x += sizeof(long);
    return (uchar *)&SerialiseWB_Buf;
}
int deserialiseWB(uchar *x, wob_t *wb) {
    uchar *ox = x;
    memcpy(&wb->nob, x, sizeof(int));         x += sizeof(int);
    if (!wb->nob) return (int)(x - ox);
    for (uint32 i = 0; i < wb->nob; i++) {
        memcpy(&wb->obt[i], x, sizeof(int));  x += sizeof(int);
        memcpy(&wb->obc[i], x, sizeof(int));  x += sizeof(int);
        memcpy(&wb->asc[i], x, sizeof(bool)); x += sizeof(bool);
    }
    memcpy(&wb->lim,  x, sizeof(long));       x += sizeof(long);
    memcpy(&wb->ofst, x, sizeof(long));       x += sizeof(long);
    return (int)(x - ox);
}

//TODO move to wc.c
void init_check_sql_where_clause(cswc_t *w, int tmatch, sds token) {
    bzero(w, sizeof(cswc_t));
    w->wtype     = SQL_ERR_LKP;
    initFilter(&w->wf);                                  /* DESTROY ME 065 */
    w->wf.tmatch = tmatch; //TODO tmatch not needed here, cuz promoteKLorFLtoW()
    w->token     = token;
}
void destroyINLlist(list **inl) {
    if (*inl) { (*inl)->free = destroyAobj; listRelease(*inl); *inl = NULL; }
}
void releaseFlist(list **flist) {
    if (*flist) { (*flist)->free = NULL; listRelease(*flist); *flist = NULL; }
}
void destroyFlist(list **flist) {
    if (*flist) {
        (*flist)->free = destroyFilter; listRelease(*flist); *flist = NULL;
    }
}
void destroy_check_sql_where_clause(cswc_t *w) {
    releaseFilterD_KL(&w->wf);                           /* DESTROYED 065 */
    destroyFlist     (&w->flist);
    if (w->lvr) sdsfree(w->lvr);
}

bool leftoverParsingReply(redisClient *c, char *x) {
    if (!x) return 1;
    while (ISBLANK(*x)) x++;
    if (*x) {
        addReplySds(c, P_SDS_EMT "-ERR could not parse '%s'\r\n", x)); return 0;
    }
    return 1;
}

// LFCA LFCA LFCA LFCA LFCA LFCA LFCA LFCA LFCA LFCA LFCA LFCA LFCA LFCA
void initLFCA(lfca_t *lfca, list *ls) {
    bzero(lfca, sizeof(lfca_t));
    if (!ls->len) return;
    int       n  = 0; listNode *ln;
    lfca->l      = malloc(sizeof(lue_t *) * ls->len);    // FREE ME 117
    listIter *li = listGetIterator(ls, AL_START_HEAD);
    while((ln = listNext(li))) {
        lfca->l[n] = (lue_t *)ln->value; n++;
    } listReleaseIterator(li);
}
void releaseLFCA(lfca_t *lfca) {
    free(lfca->l);                                       // FREED 117
}

// SELECT SELECT SELECT SELECT SELECT SELECT SELECT SELECT SELECT SELECT
bool sqlSelectBinary(cli  *c,     int     tmatch, bool   cstar, icol_t *ics,
                     int   qcols, cswc_t *w,      wob_t *wb,    bool    need_cn,
                     lfca_t *lfca) {
    (void) need_cn; // compiler warning
    if (cstar && wb->nob) { /* SELECT COUNT(*) ORDER BY -> stupid */
        addReply(c, shared.orderby_count);                          return 0;
    }
    if      (c->Prepare) { prepareRQ(c, w, wb, cstar, qcols, ics);  return 1; }
    else if (c->Explain) { explainRQ(c, w, wb, cstar, qcols, ics);  return 1; }
    //dumpW(printf, w); dumpWB(printf, wb);

#ifdef EMBEDDED_VERSION
    if (EREDIS && (need_cn || GlobalNeedCn)) {
        embeddedSaveSelectedColumnNames(tmatch, ics, qcols);
    }
#endif
    if (w->wtype != SQL_SINGLE_LKP) { /* FK, RQ, IN */
        if (w->wf.imatch == -1) {
            addReply(c, shared.rangequery_index_not_found);         return 0;
        }
        if (w->wf.imatch == Tbl[tmatch].lrui) c->LruColInSelect = 1;
        if (w->wf.imatch == Tbl[tmatch].lfui) c->LfuColInSelect = 1;
        iselectAction(c, w, wb, ics, qcols, cstar, lfca);
    } else {                         /* SQL_SINGLE_LKP */
        bt    *btr   = getBtr(w->wf.tmatch);
        aobj  *apk   = &w->wf.akey;
        dwm_t  dwm   = btFindD(btr, apk);
        if (dwm.miss) { addReply(c, shared.dirty_miss);             return 1; }
        if (cstar)    { addReply(c, shared.cone);                   return 1; }
        void  *rrow  = dwm.k;
        bool   gost  = IS_GHOST(btr, rrow);
        //printf("rrow: %p gost: %d\n", (void *)rrow, gost);
        if (gost || !rrow) { addReply(c, shared.czero);             return 1; }
        bool   hf    = 0;
        bool   ret   = passFilts(btr, apk, rrow, w->flist, tmatch, &hf);
        if (hf)                                                     return 0;
        if (!ret) { addReply(c, shared.czero);                      return 1; }
        uchar ost = OR_NONE;
        robj *r = outputRow(btr, rrow, qcols, ics, apk, tmatch, lfca, &ost);
        if (ost == OR_ALLB_OK)   { addReply(c, shared.cone);        return 1; }
        if (ost == OR_ALLB_NO)   { addReply(c, shared.czero);       return 1; }
        if (ost == OR_LUA_FAIL)  {
            addReply(c, server.alc.CurrError);                      return 0;
        }
        if (!r)                  { addReply(c, shared.nullbulk);    return 1; }
        if (!EREDIS) {
            sds   s = startOutput(1); 
            robj *o = createObject(REDIS_STRING, s);
            addReply(c, o); decrRefCount(o);
            outputColumnNames(c, tmatch, cstar, ics, qcols, lfca);
        }
        GET_LRUC GET_LFUC
        if (!addReplyRow(c, r, tmatch, apk, lruc, lrud, lfuc, lfu)) return 0;
        decrRefCount(r);
        if (wb->ovar) incrOffsetVar(c, wb, 1);
    }
    return 1;
}
bool sqlSelectInnards(cli *c,       sds  clist, sds from, sds tlist, sds where,
                      sds  wclause, bool chk,   bool need_cn) {
    CREATE_CS_LS_LIST(1)
    bool cstar = 0; bool join = 0; int qcols = 0; int tmatch  = -1;
    if (!parseSelect(c, 0, NULL, &tmatch, cmatchl, ls, &qcols, &join, &cstar,
                     clist, from, tlist, where, chk)) {
                RELEASE_CS_LS_LIST                                  return 0;
    }
    if (join) { RELEASE_CS_LS_LIST
        return doJoin(c, clist, tlist, wclause); //TODO joinBinary() w/ need_cn
    }
    CMATCHS_FROM_CMATCHL
    lfca_t lfca; initLFCA(&lfca, ls); bool ret = 0;

    c->LruColInSelect = initLRUCS(tmatch, ics, qcols);
    c->LfuColInSelect = initLFUCS(tmatch, ics, qcols);
    cswc_t w; wob_t wb;
    init_check_sql_where_clause(&w, tmatch, wclause); init_wob(&wb);
    parseWCplusQO(c, &w, &wb, SQL_SELECT);
    if (w.wtype == SQL_ERR_LKP)                                     goto sel_e;
    if (!leftoverParsingReply(c, w.lvr))                            goto sel_e;
    ret = sqlSelectBinary(c, tmatch, cstar, ics, qcols, &w, &wb,
                          need_cn, &lfca);

sel_e:
    if (!cstar) resetIndexPosOn(qcols, ics);
    destroy_wob(&wb); destroy_check_sql_where_clause(&w);
    RELEASE_CS_LS_LIST releaseLFCA(&lfca);
    return ret;
}

void sqlSelectCommand(redisClient *c) {
    if (c->argc == 2) { selectCommand(c); return; } // REDIS SELECT command
    if (c->argc != 6) { addReply(c, shared.selectsyntax); return; }
    sqlSelectInnards(c, c->argv[1]->ptr, c->argv[2]->ptr, c->argv[3]->ptr,
                        c->argv[4]->ptr, c->argv[5]->ptr, 1, 0);
}

/* DELETE DELETE DELETE DELETE DELETE DELETE DELETE DELETE DELETE DELETE */
bool deleteInnards(cli *c, sds tlist, sds wclause) {
    TABLE_CHECK_OR_REPLY(tlist,0)
    cswc_t w; wob_t wb; bool ret = 0;
    init_check_sql_where_clause(&w, tmatch, wclause); init_wob(&wb);
    parseWCplusQO(c, &w, &wb, SQL_DELETE);
    if (w.wtype == SQL_ERR_LKP)                             goto delete_cmd_end;
    if (!leftoverParsingReply(c, w.lvr))                    goto delete_cmd_end;
    //dumpW(printf, &w); dumpWB(printf, &wb);
    if (w.wtype != SQL_SINGLE_LKP) { /* FK, RQ, IN */
        if (w.wf.imatch == -1) {
            addReply(c, shared.rangequery_index_not_found); goto delete_cmd_end;
        }
        ideleteAction(c, &w, &wb);
    } else {                         /* SQL_SINGLE_DELETE */
        MATCH_INDICES(w.wf.tmatch)
        int del = deleteRow(w.wf.tmatch, &w.wf.akey, matches, inds);
        if (del == -1) addReply(c, shared.deletemiss);
        else           addReply(c, del ? shared.cone : shared.czero);
        if (wb.ovar) incrOffsetVar(c, &wb, 1);
    }
    ret = 1;

delete_cmd_end:
    destroy_wob(&wb); destroy_check_sql_where_clause(&w); return ret;
}
void deleteCommand(redisClient *c) {
    if (strcasecmp(c->argv[1]->ptr, "FROM")) {
        addReply(c, shared.deletesyntax); return;
    }
    if (strcasecmp(c->argv[3]->ptr, "WHERE")) {
        addReply(c, shared.deletesyntax_nowhere); return;
    }
    deleteInnards(c, c->argv[2]->ptr, c->argv[4]->ptr);
}

/* UPDATE UPDATE UPDATE UPDATE UPDATE UPDATE UPDATE UPDATE UPDATE UPDATE */
static int getPkUpdateCol(int qcols, icol_t *ics) {
    int pkupc = -1; /* PK UPDATEs that OVERWRITE rows disallowed */
    for (int i = 0; i < qcols; i++) {
        if (!ics[i].cmatch) { pkupc = i; break; }
    }
    return pkupc;
}
static bool assignMisses(cli   *c,      int     tmatch,   int    ncols,
                         int   qcols,   icol_t *ics,      icol_t chit[],
                         char *vals[],  uint32  vlens[],  ue_t   ue[],
                         char *mvals[], uint32  mvlens[], lue_t  le[]) {
    for (int i = 0; i < ncols; i++) {
        ue[i].yes = 0; bzero(&le[i], sizeof(lue_t));
    }
    for (int i = 0; i < ncols; i++) {
        INIT_ICOL(chit[i], -1)
        uchar ctype = Tbl[tmatch].col[i].type;
        for (int j = 0; j < qcols; j++) {
            icol_t ic = ics[j];
            if (i == ic.cmatch) {
                cloneIC(&chit[i], &ic);                  // FREE 170
                vals[i] = mvals[j]; vlens[i] = mvlens[j];
                if        C_IS_O(ctype) {
                    char *begc = vals[i]; char *endc = begc + vlens[i] - 1;
                    if (*begc == '{' || *endc == '}')                  break;
                } else if (C_IS_I(ctype) || C_IS_L(ctype)) {
                    if (getExprType(vals[i], vlens[i]) == UETYPE_INT)  break;
                } else if C_IS_X(ctype) {
                    if (getExprType(vals[i], vlens[i]) == UETYPE_U128) break;
                } else if C_IS_F(ctype) {
                    if (getExprType(vals[i], vlens[i]) == UETYPE_FLT)  break;
                } else if (C_IS_S(ctype) || C_IS_O(ctype)) {
                    if (is_text(vals[i], vlens[i]))                    break;
                }
                if C_IS_X(ctype) { // update expr's (lua 2) not allowed on U128
                    addReply(c, shared.update_u128_complex); return 0;
                }
                int k = parseExpr(c, tmatch, ic.cmatch,
                                  vals[i], vlens[i], &ue[i]);
                if (k == -1) return 0;
                if (k) { ue[i].yes = 1; break; }
                if (!parseLuaExpr(tmatch, vals[i], vlens[i], &le[i])) {
                    addReply(c, shared.updatesyntax); return 0;
                }
                break;
            }
        }
    }
    return 1;
}
static bool ovwrPKUp(cli    *c,        int    pkupc, char *mvals[],
                     uint32  mvlens[], uchar  pktyp, bt   *btr) {
    aobj  *ax  = createAobjFromString(mvals[pkupc], mvlens[pkupc], pktyp);
    dwm_t  dwm = btFindD(btr, ax); destroyAobj(ax);
    if (dwm.k || dwm.miss) { addReply(c, shared.update_pk_ovrw); return 1; }
    return 0;
}
//TODO check DNI's
static bool isUpdatingIndex(int matches, int   inds[], icol_t chit[], 
                          bool *mci_up,  bool *u_up) {
    bool ret = 0; *u_up = 0; *mci_up = 0;
    for (int i = 0; i < matches; i++) {
        r_ind_t *ri = &Index[inds[i]];
        if        (ri->virt)  { continue;
        } else if (ri->hlt)   { ret = 1;
        } else if (ri->clist) {
            for (int i = 0; i < ri->nclist; i++) {
                if (chit[ri->bclist[i].cmatch].cmatch != -1) {
                    ret = 1; if UNIQ(ri->cnstr) *mci_up = 1;
                }
            }
        } else if (chit[ri->icol.cmatch].cmatch != -1) {
            ret = 1; if UNIQ(ri->cnstr) *u_up = 1;
        }
    }
    return ret;
}
int updateInnards(cli *c,      int   tmatch, sds vallist, sds wclause,
                  bool fromup, aobj *u_apk) {
    //printf("updateInnards: vallist: %s wclause: %s\n", vallist, wclause);
    CREATE_CS_LS_LIST(0);
    list   *mvalsl  = listCreate(); list *mvlensl = listCreate();
    int     qcols   = parseUpdateColListReply(c,      tmatch, vallist, cmatchl,
                                              mvalsl, mvlensl);
    UPDATES_FROM_UPDATEL
    if (!qcols)                                                   return -1;
    for (int i = 0; i < qcols; i++) {
        if (ics[i].cmatch < -1) { addReply(c, shared.updateipos); return -1; }
    }
    if (initLRUCS(tmatch, ics, qcols)) {
        addReply(c, shared.update_lru);                           return -1;
    }
    int pkupc = getPkUpdateCol(qcols, ics);
    MATCH_INDICES(tmatch)

    /* Figure out which columns get updated(HIT) and which dont(MISS) */
    r_tbl_t *rt    = &Tbl[tmatch];
    int      ncols = rt->col_count;
    icol_t   chit[ncols]; ue_t    ue   [ncols]; lue_t le[ncols];
    char    *vals[ncols]; uint32  vlens[ncols];
    if (!assignMisses(c, tmatch, ncols, qcols, ics, chit, vals, vlens, ue,
                      mvals, mvlens, le))                      return -1;
    int nsize = -1; /* B4 GOTO */
    cswc_t w; wob_t wb; init_wob(&wb);
    if (fromup) { /* comes from "INSERT ON DUPLICATE KEY UPDATE" */
        init_check_sql_where_clause(&w, tmatch, NULL);           /* ERR->GOTO */
        w.wtype     = SQL_SINGLE_LKP; /* JerryRig WhereClause to "pk = X" */
        w.wf.imatch = rt->vimatch;    /* pk index */
        w.wf.tmatch = tmatch;         /* table from INSERT UPDATE */
        w.wf.akey   = *u_apk;         /* PK from INSERT UPDATE */
    } else {      /* normal UPDATE -> parse WhereClause */
        init_check_sql_where_clause(&w, tmatch, wclause);/* ERR->GOTO */
        parseWCplusQO(c, &w, &wb, SQL_UPDATE);
        if (w.wtype == SQL_ERR_LKP)                              goto upc_end;
        if (!leftoverParsingReply(c, w.lvr))                     goto upc_end;
    } //dumpW(printf, &w); dumpWB(printf, &wb);

    bool  u_up, mci_up; 
    bool  upi  = isUpdatingIndex(matches, inds, chit, &mci_up, &u_up);
    bt   *btr  = getBtr(w.wf.tmatch);
    bool  isr  = (w.wtype != SQL_SINGLE_LKP);
    if (mci_up && isr) { addReply(c, shared.range_mciup);        goto upc_end; }
    if (u_up   && isr) { addReply(c, shared.range_u_up);         goto upc_end; }

    if (isr) { /* FK, RQ, IN -> RANGE UPDATE */
        if (pkupc != -1) {
            addReply(c, shared.update_pk_range_query);           goto upc_end;
        }
        if (w.wf.imatch == -1) {
            addReply(c, shared.rangequery_index_not_found);      goto upc_end;
        }
        iupdateAction(c,  &w, &wb, ncols, matches, inds, vals, vlens, chit,
                      ue, le, upi);
    } else {   /* SQL_SINGLE_UPDATE */
        uchar  pktyp = rt->col[0].type;
        if (pkupc != -1) { /* disallow pk updts that overwrite other rows */
            if (ovwrPKUp(c, pkupc, mvals, mvlens, pktyp, btr))   goto upc_end;
        }
        aobj  *apk    = &w.wf.akey;
        dwm_t  dwm    = btFindD(btr, apk);
        void  *rrow   = dwm.k;
        bool   gost   = IS_GHOST(btr, rrow);
        bool   exists = (rrow || dwm.miss) && !gost;
        bool   upx    = !upi && (wb.lim == -1) && !w.flist; //DEBUG_UPDATE_SNGL
        if (!exists)  { addReply(c, shared.czero);               goto upc_end; }
        if (dwm.miss) { if (upx) addReply(c, shared.cone);
                        else     addReply(c, shared.updatemiss); goto upc_end; }
        uc_t uc;
        init_uc(&uc, btr, w.wf.tmatch, ncols, matches, inds, vals, vlens,
                chit, ue, le);
        nsize        = updateRow(c, &uc, apk, rrow, 0); release_uc(&uc);
        //NOTE: rrow is no longer valid, updateRow() can change it
        if (nsize == -1)                                         goto upc_end;
        if (!fromup) addReply(c, shared.cone);
        if (wb.ovar) incrOffsetVar(c, &wb, 1);
    }

upc_end:
    destroy_wob(&wb); destroy_check_sql_where_clause(&w);
    for (int i = 0; i < ncols; i++) releaseIC(&chit[i]); // FREED 170
    release_ics(ics, qcols);
    return nsize;
}
static int updateAction(cli *c, sds u_vallist, aobj *u_apk, int u_tmatch) {
    if (!u_vallist) {
        TABLE_CHECK_OR_REPLY(c->argv[1]->ptr, -1)
        if (strcasecmp(c->argv[2]->ptr, "SET")) {
            addReply(c, shared.updatesyntax);                  return -1;
        }
        if (strcasecmp(c->argv[4]->ptr, "WHERE")) {
            addReply(c, shared.updatesyntax_nowhere);          return -1;
        }
        u_tmatch = tmatch;
    }
    bool fromup  = u_vallist ? 1         : 0;
    sds  vallist = fromup    ? u_vallist : c->argv[3]->ptr;
    sds  wclause = fromup    ? NULL      : c->argv[5]->ptr;
    return updateInnards(c, u_tmatch, vallist, wclause, fromup, u_apk);
}
void updateCommand(redisClient *c) {
    updateAction(c, NULL, NULL, -1);
}
