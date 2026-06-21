// sid2goat — GoatTracker 2 / GTUltra SID → SNG converter
// Inverts greloc.c binary format back to editable GoatTracker source.
#include "gsong.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <map>
#include <set>
#include <climits>

// ── Pattern decode (inverse of greloc.c packpattern) ────────────────────────
// Binary layout: 0x00=END, 0x01-0x3F=instr-change, 0x40-0x4F=note+FX,
//   0x50-0x5F=REST+FX, 0x60-0xBD=note/REST, 0xC0-0xFE=packed-rest.
// CMD_SETTEMPO: binary arg = sng_arg-1 (for sng_arg≥3) → inverse: +1 if ≥2.
static int unpack_patt(uint8_t *out, const uint8_t *data, int off, int maxlen) {
    int pos=off, row=0;
    uint8_t cmd=0, arg=0;
    uint8_t cur_instr=0;
    while (row<gt::MAX_PATTROWS && pos<maxlen) {
        uint8_t b=data[pos++];
        if (b==0x00) break;
        uint8_t row_instr=0;
        if (b<gt::FX) {
            row_instr=b;
            cur_instr=b;
            if(pos>=maxlen) break;
            b=data[pos++];
            if(b==0x00) break;
        }
        if (b>=gt::FX && b<gt::FXONLY) {
            cmd=b-gt::FX; arg=(cmd&&pos<maxlen)?data[pos++]:0;
            if(cmd==0x0f&&arg>=2) arg++;
            if(pos>=maxlen) break;
            b=data[pos++];
        } else if (b>=gt::FXONLY && b<gt::FIRSTNOTE) {
            cmd=b-gt::FXONLY; arg=(cmd&&pos<maxlen)?data[pos++]:0;
            if(cmd==0x0f&&arg>=2) arg++;
            out[row*4+0]=gt::REST; out[row*4+1]=0; out[row*4+2]=cmd; out[row*4+3]=arg;
            row++; continue;
        }
        if (b>=0xC0) {
            int cnt=256-b;
            for(int r=0;r<cnt&&row<gt::MAX_PATTROWS;r++,row++) {
                out[row*4+0]=gt::REST; out[row*4+1]=0; out[row*4+2]=cmd; out[row*4+3]=arg;
            }
        } else {
            uint8_t oi;
            if (b>=gt::FIRSTNOTE && b<=gt::LASTNOTE)
                oi=row_instr?row_instr:cur_instr;
            else
                oi=(b!=gt::REST)?row_instr:0;
            out[row*4+0]=b; out[row*4+1]=oi; out[row*4+2]=cmd; out[row*4+3]=arg; row++;
        }
    }
    out[row*4+0]=gt::ENDPATT; out[row*4+1]=0; out[row*4+2]=0; out[row*4+3]=0;
    return row;
}

// ── WTBL ltable inverse (greloc.c §WTBL encode) ─────────────────────────────
static uint8_t wtbl_l_inv(uint8_t b, bool nwd) {
    if (b==0xff) return 0xff;
    if (nwd) return (b<=0x0f)?(uint8_t)(0xe0|b):b;
    if (b==0x00) return 0x00;
    if (b<=0x0f) return b;
    if (b<=0x1f) return (uint8_t)(0xe0|(b-0x10));
    if (b<0xf0)  return (uint8_t)(b-0x10);
    return b;
}
static uint8_t wtbl_r_inv(uint8_t r, uint8_t l_bin) {
    if (l_bin==0xff||(l_bin>=0xf0&&l_bin<=0xfe)) return r;
    return r^0x80;
}

// ── FTBL ltable inverse (greloc.c §FTBL encode) ─────────────────────────────
// Forward: if(L>0x80&&L!=0xFF) binary=((L&0x70)>>1)|0x80  else binary=L
static uint8_t ftbl_l_inv(uint8_t b) {
    if (b!=0xff&&b>0x80) return (uint8_t)(0x80|((b&0x7f)<<1));
    return b;
}

// ── makespeedtable (gtable.c) ───────────────────────────────────────────────
// Converts a raw speed/vibrato byte (as stored verbatim by legacy v2.0-style
// binaries, both per-instrument and as pattern cmd1-4/14 args) into an
// (l,r) STBL entry. data==0 means "no entry" (resulting index 0).
enum { MST_NOFINEVIB=0, MST_FINEVIB=1, MST_FUNKTEMPO=2, MST_PORTAMENTO=3 };
static bool make_speed_lr(uint8_t data, int mode, uint8_t &l, uint8_t &r) {
    if (!data) return false;
    switch (mode) {
        case MST_NOFINEVIB: l=(data&0xf0)>>4; r=(data&0x0f)<<4; break;
        case MST_FINEVIB:   l=(data&0x70)>>4; r=(uint8_t)(((data&0x0f)<<4)|((data&0x80)>>4)); break;
        case MST_FUNKTEMPO: l=(data&0xf0)>>4; r=data&0x0f; break;
        case MST_PORTAMENTO:{int temp=((int)data<<2)&0xffff; l=(uint8_t)(temp>>8); r=(uint8_t)(temp&0xff);} break;
        default: return false;
    }
    return true;
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    if (argc!=3) { fprintf(stderr,"Usage: sid2goat in.sid out.sng\n"); return 1; }

    FILE *fh=fopen(argv[1],"rb");
    if (!fh) { fprintf(stderr,"Cannot open %s\n",argv[1]); return 1; }
    fseek(fh,0,SEEK_END); int flen=ftell(fh); rewind(fh);
    std::vector<uint8_t> buf(flen);
    if(fread(buf.data(),1,flen,fh)!=(size_t)flen){ fprintf(stderr,"Read error\n"); fclose(fh); return 1; } fclose(fh);
    const uint8_t *sid=buf.data();

    if (memcmp(sid,"PSID",4)&&memcmp(sid,"RSID",4)) { fprintf(stderr,"Not a PSID/RSID file\n"); return 1; }
    int hdr=(sid[6]<<8)|sid[7];
    int la=(sid[8]<<8)|sid[9];
    int ds;
    if (la==0) { la=sid[hdr]|(sid[hdr+1]<<8); ds=hdr+2; }
    else         ds=hdr;
    auto s2f=[&](int s){ return s-la+ds; };
    auto f2s=[&](int f){ return f-ds+la; };
    auto inb=[&](int f){ return f>=0&&f<flen; };

    const uint8_t *code=sid+ds;
    int code_len=flen-ds;

    // ── Scan all LDA abs,Y in player code ────────────────────────────────────
    std::map<int,int> acnt;   // sid_addr → access count
    std::set<int>     aset;
    std::vector<std::pair<int,int>> ldas; // (code_off, sid_addr)
    for (int i=0;i<code_len-2;i++) {
        if (code[i]==0xb9) {
            int a=code[i+1]|(code[i+2]<<8);
            if (a>=la&&a<la+code_len) { ldas.push_back({i,a}); aset.insert(a); acnt[a]++; }
            i+=2;
        }
    }
    (void)aset; (void)acnt;

    // ── Locate instrument cluster (column-major table in binary) ─────────────
    // Each column is an array of N_INSTR bytes accessed via LDA col_base,Y.
    // Columns are evenly spaced (gap = N_INSTR) and appear as a run in the
    // sorted LDA-address list. We find the longest such run.
    std::map<int,int> sa_best;
    for (auto&[co,sa]:ldas) if(sa>la+0x100) sa_best[sa]=std::max(sa_best[sa],co);
    std::vector<std::pair<int,int>> da;
    for (auto&[sa,co]:sa_best) da.push_back({sa,co});
    std::sort(da.begin(),da.end());

    int best_s=0,best_g=1,best_n=0;
    for (int s=0;s<(int)da.size()-2;s++) {
        int g=da[s+1].first-da[s].first;
        if(g<=0||g>64) continue;
        // Extend the run only while the running (cmin,cmax) window over
        // included code-offsets stays within range -- a coincidentally
        // address-aligned byte belonging to an unrelated table (e.g. an
        // effect/speed lookup elsewhere in the code) will typically have a
        // code offset far from the genuine column cluster, and should stop
        // the run rather than invalidate it.
        int n=1,cmin=da[s].second,cmax=da[s].second;
        while(s+n<(int)da.size()&&da[s+n].first-da[s+n-1].first==g){
            int nc=da[s+n].second;
            int ncmin=std::min(cmin,nc), ncmax=std::max(cmax,nc);
            if(ncmax-ncmin>300) break;
            cmin=ncmin; cmax=ncmax; n++;
        }
        if(n>=2&&n>best_n){best_s=s;best_g=g;best_n=n;}
    }
    std::vector<std::pair<int,int>> cl;
    for(int k=0;k<best_n;k++) cl.push_back({da[best_s+k].first,da[best_s+k].second});
    std::sort(cl.begin(),cl.end(),[](auto&a,auto&b){return a.second<b.second;});
    int sp=(int)cl.size();
    for(int k=1;k<(int)cl.size();k++) if(cl[k].second-cl[k-1].second>60){sp=k;break;}
    cl.resize(sp);
    std::sort(cl.begin(),cl.end());

    int mt_wavetbl=cl.back().first+1;
    std::vector<int> icols;
    for(int k=0;k<(int)cl.size()-1;k++) icols.push_back(cl[k].first);
    int N_COLS=(int)icols.size();
    int N_INSTR=(N_COLS>1)?(icols[1]-icols[0]):best_g;
    printf("N_INSTR=%d  N_COLS=%d  mt_wavetbl=SID %04x\n",N_INSTR,N_COLS,mt_wavetbl);

    // ── Find song and pattern tables from player code ─────────────────────────
    // Signature: LDA tbl_lo,Y / STA zp / LDA tbl_hi,Y / STA zp
    // Orderlist variant has LDA (zp),Y / CMP #$FF after; pattern variant doesn't.
    int mt_songtbllo=-1,mt_songtblhi=-1,mt_patttbllo=-1,mt_patttblhi=-1;
#ifdef DEBUG_COLS
    std::vector<std::tuple<int,int,bool>> all_cands; // a1,a2,has_ol
#endif
    for(int i=0;i<code_len-20;i++){
        if(code[i]!=0xB9) continue;
        int a1=code[i+1]|(code[i+2]<<8);
        if(a1<la||a1>=la+code_len) continue;
        if(code[i+3]!=0x85) continue;
        uint8_t zp1=code[i+4];
        for(int j=i+5;j<i+14&&j+4<code_len;j++){
            if(code[j]!=0xB9) continue;
            int a2=code[j+1]|(code[j+2]<<8);
            if(a2<=a1||a2>=la+code_len) continue;
            if(code[j+3]!=0x85) continue;
            bool has_ol=false;
            for(int k=j+5;k<j+40&&k+3<code_len;k++)
                if(code[k]==0xB1&&code[k+1]==zp1&&code[k+2]==0xC9&&code[k+3]==0xFF){has_ol=true;break;}
#ifdef DEBUG_COLS
            all_cands.push_back({a1,a2,has_ol});
#endif
            if(has_ol&&mt_songtbllo<0){mt_songtbllo=a1;mt_songtblhi=a2;}
            else if(!has_ol&&mt_patttbllo<0){mt_patttbllo=a1;mt_patttblhi=a2;}
            break;
        }
    }
#ifdef DEBUG_COLS
    printf("all LDA-pair candidates (a1,a2,has_ol):\n");
    for(auto&[a1,a2,ho]:all_cands) printf("  %04x %04x %s\n",a1,a2,ho?"ol":"--");
#endif
    if(mt_songtbllo<0||mt_patttbllo<0){fprintf(stderr,"Could not find song/pattern tables in 6502 code\n");return 1;}
#ifdef DEBUG_COLS
    printf("songtbl=%04x/%04x  patttbl=%04x/%04x\n",mt_songtbllo,mt_songtblhi,mt_patttbllo,mt_patttblhi);
#endif

    int n_songs_v=(mt_songtblhi-mt_songtbllo)/3;
    int n_patts_v=mt_patttblhi-mt_patttbllo;
    int mt_insad_v=mt_patttblhi+n_patts_v;

    // ── Refine instrument cluster using definitive mt_insad ───────────────────
    {
        std::vector<int> ic2;
        for(auto&[sa,co]:cl) if(sa>=mt_insad_v-1) ic2.push_back(sa);
        if(ic2.size()>=2){
            icols=ic2; N_COLS=(int)icols.size()-1; N_INSTR=icols[1]-icols[0];
            mt_wavetbl=icols.back()+1;
            icols.pop_back();
            printf("N_INSTR=%d  N_COLS=%d  mt_wavetbl=SID %04x (refined)\n",N_INSTR,N_COLS,mt_wavetbl);
        }
    }

    if(icols.empty()){fprintf(stderr,"Could not locate instrument-cluster columns\n");return 1;}
    auto read_col=[&](int ci)->std::vector<uint8_t>{
        std::vector<uint8_t> v(N_INSTR,0);
        int base=icols[0]+ci*N_INSTR;
        for(int i=0;i<N_INSTR;i++) v[i]=sid[s2f(base+1+i)];
        return v;
    };
    std::vector<uint8_t> adc=read_col(0), srv=read_col(1), wptr=read_col(2);
#ifdef DEBUG_COLS
    {
        printf("icols:"); for(int v:icols) printf(" %04x",v); printf("\n");
        for(size_t ci=0;ci<icols.size();ci++){
            printf("  col%zu(%04x):",ci,(size_t)icols[ci]);
            for(int i=0;i<N_INSTR;i++) printf(" %02x",sid[s2f(icols[ci]+1+i)]);
            printf("\n");
        }
    }
#endif

    // ── Locate orderlist start (upper bound for table region) ─────────────────
    int ol_file=flen;
    for(int c=0;c<n_songs_v*3;c++){int lo=sid[s2f(mt_songtbllo+c)],hi=sid[s2f(mt_songtblhi+c)];int af=s2f((hi<<8)|lo);if(af>=ds&&af<flen)ol_file=std::min(ol_file,af);}
    if(ol_file>=flen){fprintf(stderr,"Bad song table addresses\n");return 1;}
    printf("Orderlists at file %04x\n",ol_file);
    int ol_SID=f2s(ol_file);
#ifdef DEBUG_COLS
    printf("ol_SID=%04x  n_songs=%d n_patts=%d mt_insad_v=%04x\n",ol_SID,n_songs_v,n_patts_v,mt_insad_v);
#endif

    // ── Unpack ALL patterns once (independent of table addresses) ─────────────
    // Also build a histogram of table-pointer CMD args, used as minimum table
    // lengths for the content-sequential reader below.
    std::vector<std::vector<uint8_t>> patt_data(gt::MAX_PATT, std::vector<uint8_t>(gt::MAX_PATTROWS*4+4,0));
    std::vector<int> patt_len(gt::MAX_PATT,0);
    int highest_patt=-1;
    int max_wptr_cmd=0,max_pptr_cmd=0,max_fptr_cmd=0,max_stbl_cmd=0;
    for(int p=0;p<n_patts_v&&p<gt::MAX_PATT;p++){
        int lo=sid[s2f(mt_patttbllo+p)],hi=sid[s2f(mt_patttblhi+p)];
        int pf=s2f((hi<<8)|lo);
        if(pf<ds||pf>=flen){patt_len[p]=0;continue;}
        for(auto&b:patt_data[p]) b=0;
        int rows=unpack_patt(patt_data[p].data(),sid,pf,flen);
        patt_len[p]=rows; highest_patt=p;
        for(int r=0;r<rows;r++){
            uint8_t cmd=patt_data[p][r*4+2], arg=patt_data[p][r*4+3];
            switch(cmd){
                case 8:  max_wptr_cmd=std::max(max_wptr_cmd,(int)arg); break;
                case 9:  max_pptr_cmd=std::max(max_pptr_cmd,(int)arg); break;
                case 10: max_fptr_cmd=std::max(max_fptr_cmd,(int)arg); break;
                case 1: case 2: case 3: case 4: case 14:
                         max_stbl_cmd=std::max(max_stbl_cmd,(int)arg); break;
                default: break;
            }
        }
    }
#ifdef DEBUG_COLS
    printf("max_wptr_cmd=%d max_pptr_cmd=%d max_fptr_cmd=%d max_stbl_cmd=%d highest_patt=%d\n",
           max_wptr_cmd,max_pptr_cmd,max_fptr_cmd,max_stbl_cmd,highest_patt);
#endif

    // ── Jump-consistent table reader (WTBL/PTBL/FTBL) ──────────────────────────
    // Per gtable.c's exectable()/gettablepartlen(): each table is a sequence of
    // "parts", each ending in an (0xFF,R) entry where R=0 means stop and R!=0 is
    // a 1-indexed jump *within the compacted table* (R must be in [0,N]). The
    // table's total length N is the smallest N>=min_len such that ltable[N-1]
    // ==0xFF and every internal (0xFF,R) entry has R in [0,N]. L and R arrays
    // are both exactly N bytes (R immediately follows L).
    auto find_jump_consistent_len=[&](int start,int min_len,int limit,
                                       std::vector<uint8_t>&rawL,std::vector<uint8_t>&rawR)->int{
        if(min_len<1) min_len=1;
        for(int N=min_len;2*N<=limit;N++){
            int lf=s2f(start+N-1); if(!inb(lf)) break;
            if(sid[lf]!=0xff) continue;
            bool ok=true;
            for(int k=1;k<=N&&ok;k++){
                int klf=s2f(start+k-1); if(!inb(klf)){ok=false;break;}
                if(sid[klf]==0xff){
                    int krf=s2f(start+N+k-1); if(!inb(krf)){ok=false;break;}
                    int rk=sid[krf];
                    if(rk<0||rk>N) ok=false;
                }
            }
            if(ok){
                rawL.assign(N,0); rawR.assign(N,0);
                for(int i=0;i<N;i++){rawL[i]=sid[s2f(start+i)]; rawR[i]=sid[s2f(start+N+i)];}
                return N;
            }
        }
        return -1;
    };

    struct Combo {
        bool nopulse,nofilter,noinsvib,fixedparams;
        std::vector<uint8_t> pptr,filtpt,vibp,vibd,gatec,firstc;
        std::vector<uint8_t> wtbl_L,wtbl_R,ptbl_L,ptbl_R,ftbl_L,ftbl_R,stbl_L,stbl_R;
        int N_WTBL=0,N_PTBL=0,N_FTBL=0,N_STBL=0,total=0,diff=INT_MAX;
        int ncols_try=0;
        bool legacy_format=false;
    };
    std::vector<Combo> results;
    int icols0=icols[0];
    // 9 is the theoretical maximum: 3 always-present columns (AD,SR,wave)
    // plus up to 6 optional ones (pulse,filter,vibptr+vibdelay,gatetimer+
    // firstwave). Some binaries reserve the full fixed-size instrument
    // layout regardless of which fields any code path actually touches, so
    // a real column can exist with no live LDA reference to find it by.
    for(int ncols_try=N_COLS; ncols_try<=9; ncols_try++){
        int mt_wavetbl_try=icols0+ncols_try*N_INSTR+1;
        // The table region ends at whichever known structure (songtbl/patttbl
        // pointer arrays, or the orderlist data itself) comes first after
        // mt_wavetbl_try. In some layouts (e.g. early relocators) the pointer
        // arrays sit between the tables and the orderlist data; in others they
        // precede the instrument cluster entirely and don't constrain this.
        // When a pointer array is the binding constraint, this is the older
        // (v2.0-style) relocator layout, which also stores WTBL/FTBL verbatim
        // (no XOR/wave-delay/filter-cutoff transforms) and has no STBL section.
        int region_end=ol_SID;
        bool legacy_format=false;
        for(int addr:{mt_songtbllo,mt_songtblhi,mt_patttbllo,mt_patttblhi})
            if(addr>mt_wavetbl_try&&addr<region_end){region_end=addr;legacy_format=true;}
        int total_avail=region_end-mt_wavetbl_try;
        if(total_avail<=0) continue;

        int max_w=0; for(auto v:wptr) max_w=std::max(max_w,(int)v); max_w=std::max(max_w,max_wptr_cmd);
        std::vector<uint8_t> wtbl_L,wtbl_R;
        int nw=find_jump_consistent_len(mt_wavetbl_try,max_w,total_avail,wtbl_L,wtbl_R);
#ifdef DEBUG_COLS
        printf("ncols_try=%d mt_wavetbl=%04x total_avail=%d max_w=%d nw=%d\n",
               ncols_try,mt_wavetbl_try,total_avail,max_w,nw);
#endif
        if(nw<0) continue;
        int pos1=mt_wavetbl_try+2*nw;

        for(int a=0;a<=1;a++) for(int b=0;b<=1;b++) for(int c=0;c<=1;c++) for(int d=0;d<=1;d++){
            if(3+a+b+2*c+2*d!=ncols_try) continue;
            Combo R;
            R.wtbl_L=wtbl_L; R.wtbl_R=wtbl_R; R.N_WTBL=nw;
            int ci=3;
            R.pptr  = a? read_col(ci++) : std::vector<uint8_t>(N_INSTR,0);
            R.filtpt= b? read_col(ci++) : std::vector<uint8_t>(N_INSTR,0);
            if(c){ R.vibp=read_col(ci++); R.vibd=read_col(ci++); } else { R.vibp.assign(N_INSTR,0); R.vibd.assign(N_INSTR,0); }
            if(d){ R.gatec=read_col(ci++); R.firstc=read_col(ci++); } else { R.gatec.assign(N_INSTR,0); R.firstc.assign(N_INSTR,0); }

            int max_p=0; for(auto v:R.pptr) max_p=std::max(max_p,(int)v); max_p=std::max(max_p,max_pptr_cmd);
            int max_f=0; for(auto v:R.filtpt) max_f=std::max(max_f,(int)v); max_f=std::max(max_f,max_fptr_cmd);
            int max_s=0; for(auto v:R.vibp) max_s=std::max(max_s,(int)v); max_s=std::max(max_s,max_stbl_cmd);

            // STBL "extra zero" pair (one before L, one before R) is emitted
            // only when the song actually uses vibrato/portamento/toneporta/
            // funktempo (greloc.c: (!novib)||(!nofunktempo)||(!noportamento)||
            // (!notoneporta)) -- approximated here by max_s>0 (an instrument's
            // STBL pointer or a pattern cmd1-4/14 arg references the table).
            // Legacy (v2.0-style) binaries have no STBL section at all.
            int stbl_zeros=(!legacy_format&&max_s>0)?2:0;

            // Table EXISTENCE (nopulse/nofilter) is content-derived: a column
            // can exist in the instrument-data layout (a/b above) yet still be
            // all-zero, in which case greloc.c never emits that table at all.
            R.nopulse=(max_p==0); R.nofilter=(max_f==0);
            R.noinsvib=!c; R.fixedparams=!d;

            int pos=pos1;
            int np=0,nf=0;
            std::vector<uint8_t> ptbl_L,ptbl_R,ftbl_L,ftbl_R;
            if(!R.nopulse){
                np=find_jump_consistent_len(pos,max_p,region_end-pos,ptbl_L,ptbl_R);
#ifdef DEBUG_COLS
                printf("  a=%d b=%d c=%d d=%d pos=%04x max_p=%d np=%d\n",a,b,c,d,pos,max_p,np);
#endif
                if(np<0) continue;
                pos+=2*np;
            }
            if(!R.nofilter){
                nf=find_jump_consistent_len(pos,max_f,region_end-pos,ftbl_L,ftbl_R);
#ifdef DEBUG_COLS
                printf("  a=%d b=%d c=%d d=%d pos=%04x max_f=%d nf=%d\n",a,b,c,d,pos,max_f,nf);
#endif
                if(nf<0) continue;
                pos+=2*nf;
            }
            int remaining=region_end-pos-stbl_zeros;
            if(remaining<0||remaining%2!=0) continue;
            int ns=remaining/2;
            R.ptbl_L=ptbl_L; R.ptbl_R=ptbl_R; R.ftbl_L=ftbl_L; R.ftbl_R=ftbl_R;
            R.N_PTBL=np; R.N_FTBL=nf; R.N_STBL=ns;
            R.stbl_L.assign(ns,0); R.stbl_R.assign(ns,0);
            for(int i=0;i<ns;i++){int f=s2f(pos+stbl_zeros/2+i);if(inb(f))R.stbl_L[i]=sid[f];
                                   int g=s2f(pos+stbl_zeros+ns+i);if(inb(g))R.stbl_R[i]=sid[g];}
            R.ncols_try=ncols_try; R.legacy_format=legacy_format;
            R.total=2*(nw+np+nf+ns)+stbl_zeros; R.diff=0;
#ifdef DEBUG_COLS
            printf("  combo a=%d b=%d c=%d d=%d nopulse=%d nofilter=%d : N=%d,%d,%d,%d stbl_zeros=%d max_s=%d\n",
                   a,b,c,d,(int)R.nopulse,(int)R.nofilter,R.N_WTBL,R.N_PTBL,R.N_FTBL,R.N_STBL,stbl_zeros,max_s);
#endif
            results.push_back(R);
        }
    }
    if(results.empty()){fprintf(stderr,"No self-consistent table layout found\n");return 1;}
#ifdef DEBUG_COLS
    printf("max_cmd: wptr=%d pptr=%d fptr=%d stbl=%d  (results=%zu)\n",max_wptr_cmd,max_pptr_cmd,max_fptr_cmd,max_stbl_cmd,results.size());
    for(auto&R:results) printf("  combo ncols=%d nopulse=%d nofilter=%d noinsvib=%d fixedparams=%d : N=%d,%d,%d,%d\n",
        R.ncols_try,(int)R.nopulse,(int)R.nofilter,(int)R.noinsvib,(int)R.fixedparams,R.N_WTBL,R.N_PTBL,R.N_FTBL,R.N_STBL);
#endif
    // ── Detect firstwave_evidence from player code ─────────────────────────────
    // Find: LDA chnwave,X; AND chngate,X; STA $D404,X (ZP or abs indexed)
    // Then find what writes to chnwave: LDA abs,Y (B9 = table → real column,
    // supports noinsvib=true) or LDA #imm (A9 → fixedparams).
    int firstwave_evidence=-1; // -1=unknown, 0=fixedparams(LDA#imm), 1=table(LDA abs,Y)
    // ZP-indexed form: B5 zp; 35 zp; 9D 04 D4
    for(int i=0;i<code_len-6&&firstwave_evidence<0;i++){
        if(code[i]==0xB5&&code[i+2]==0x35&&
           code[i+4]==0x9D&&code[i+5]==0x04&&code[i+6]==0xD4){
            uint8_t cw=code[i+1];
            for(int m=2;m<code_len-1;m++){
                if(code[m]==0x95&&code[m+1]==cw){
                    if(m>=3&&code[m-3]==0xB9) firstwave_evidence=1;
                    else if(m>=5&&code[m-5]==0xB9&&code[m-2]==0xF0) firstwave_evidence=1;
                    else if(m>=2&&code[m-2]==0xA9) firstwave_evidence=0;
                    else if(m>=4&&code[m-4]==0xA9&&code[m-2]==0xF0) firstwave_evidence=0;
                    break;
                }
            }
        }
    }
    // Abs-indexed form: BD lo hi; 3D lo2 hi2; 9D lo3 hi3
    // Accept any STA abs,X destination (not just $D404,X) because some players
    // write through a RAM shadow that is bulk-copied to SID via a loop.
    for(int i=0;i<code_len-8&&firstwave_evidence<0;i++){
        if(code[i]==0xBD&&code[i+3]==0x3D&&code[i+6]==0x9D){
            uint8_t cw_lo=code[i+1], cw_hi=code[i+2];
            for(int m=3;m<code_len-2;m++){
                if(code[m]==0x9D&&code[m+1]==cw_lo&&code[m+2]==cw_hi){
                    if(m>=3&&code[m-3]==0xB9) firstwave_evidence=1;
                    else if(m>=5&&code[m-5]==0xB9&&code[m-2]==0xF0) firstwave_evidence=1;
                    else if(m>=2&&code[m-2]==0xA9) firstwave_evidence=0;
                    else if(m>=4&&code[m-4]==0xA9&&code[m-2]==0xF0) firstwave_evidence=0;
                    break;
                }
            }
        }
    }
    printf("firstwave_evidence=%d\n",firstwave_evidence);

    // ── Detect ptbl_evidence from player code ──────────────────────────────────
    // The PTBL is processed per-channel (X-register = channel offset), so any
    // LDA (ptbl_start-1),Y in the player will be surrounded by abs,X instructions
    // (BD/9D/BC). The FTBL is processed globally (no X indexing). Use this to
    // break the tie between nopulse=false (PTBL at WTBL_end) and nopulse=true
    // (no PTBL, FTBL starts at WTBL_end) when both are structurally valid.
    int ptbl_evidence=-1; // -1=unknown, 0=no real PTBL, 1=real PTBL at WTBL_end
    for(auto&r:results){
        if(r.nopulse) continue; // only check combos claiming a PTBL
        int ptbl_start=icols0+r.ncols_try*N_INSTR+1+2*r.N_WTBL;
        uint8_t tlo=(ptbl_start-1)&0xff, thi=((ptbl_start-1)>>8)&0xff;
        for(int i=0;i<code_len-2&&ptbl_evidence<0;i++){
            if(code[i]==0xB9&&code[i+1]==tlo&&code[i+2]==thi){
                int lo=std::max(0,i-8),hi=std::min(code_len-1,i+8);
                for(int j=lo;j<=hi;j++)
                    if(code[j]==0xBD||code[j]==0x9D||code[j]==0xBC){ptbl_evidence=1;break;}
                if(ptbl_evidence<0) ptbl_evidence=0;
            }
        }
    }
    printf("ptbl_evidence=%d\n",ptbl_evidence);

    std::sort(results.begin(),results.end(),[&](const Combo&x,const Combo&y){
        // Prefer layouts that actually populate PTBL/FTBL from real column
        // data over degenerate (nopulse&&nofilter) layouts that dump the same
        // bytes into a larger STBL -- the latter only "fits" because a=0,b=0
        // makes max_p=max_f=0 by construction, regardless of column content.
        int xt=(!x.nopulse)+(!x.nofilter), yt=(!y.nopulse)+(!y.nofilter);
        if(xt!=yt) return xt>yt;
        // Use disassembly evidence to distinguish pulse (per-channel, X-indexed)
        // from filter (global) table when total optional-table count is equal.
        if(x.nopulse!=y.nopulse){
            if(ptbl_evidence==1) return !x.nopulse; // prefer nopulse=false (real PTBL)
            if(ptbl_evidence==0) return x.nopulse;  // prefer nopulse=true (no PTBL)
        }
        if(x.noinsvib!=y.noinsvib){
            // evidence=1 (table read → real firstwave column) → prefer noinsvib=true
            if(firstwave_evidence==1) return x.noinsvib>y.noinsvib;
            return x.noinsvib<y.noinsvib; // default: prefer noinsvib=false
        }
        return false;
    });
    Combo &best=results[0];
    printf("Layout: nopulse=%d nofilter=%d noinsvib=%d fixedparams=%d (diff=%d)\n",
           (int)best.nopulse,(int)best.nofilter,(int)best.noinsvib,(int)best.fixedparams,best.diff);
    printf("WTBL: N=%d  PTBL: N=%d  FTBL: N=%d  STBL: N=%d\n",best.N_WTBL,best.N_PTBL,best.N_FTBL,best.N_STBL);

    // ── Legacy-format STBL synthesis ────────────────────────────────────────
    // v2.0-style binaries store NO compiled STBL: per-instrument vibrato is a
    // raw speed byte, and pattern cmd1-4/14 (PORTAUP/PORTADOWN/TONEPORTA/
    // VIBRATO/FUNKTEMPO) args are raw speed bytes too. The loader converts
    // these via makespeedtable(data,mode,makenew=0) -- dedup by (l,r), data==0
    // means no entry (resulting index 0). We replicate that here so the
    // GTS5 output has a real STBL and STBL-index pattern args/instrument ptrs.
    //
    // NOTE: an earlier version of this code (incorrectly) concluded that
    // PORTAUP/PORTADOWN/TONEPORTA's real speed source was always the
    // triggering instrument's column6, not the pattern's own arg byte. That
    // was based on disassembling only the effect-0 path of this player's
    // tick0 dispatch ($131A, reached when there's no explicit pattern
    // command). What was missed: the JSR target at $11A4/$11AD is itself
    // self-modified per effect number via a jump table at $1010 (mirroring
    // GoatTracker v2.06's mt_tick0jumptbl/mt_tick0jump1+1/mt_tick0jump2+1
    // mechanism) -- effect 0 -> $131A (discards the passed arg, substitutes
    // column6), but effects 1/2 -> $1320 and effects 3/4 -> $1327, both of
    // which store the passed-in A (the pattern's own arg, loaded right
    // before the call) through untouched. So the pattern's arg byte is the
    // genuine speed source for PORTAUP/PORTADOWN/TONEPORTA/VIBRATO after
    // all, exactly as in stock GoatTracker; only the implicit "no explicit
    // command" default-vibrato case (effect 0) uses the instrument's column6
    // -- which is already handled correctly via the per-instrument STBL
    // pointer synthesized below. No playback-order simulation is needed.
    std::vector<uint8_t> legacy_vibdelay(N_INSTR,0), legacy_stblptr(N_INSTR,0);
    if(best.legacy_format){
        std::vector<std::pair<uint8_t,uint8_t>> entries;
        auto stbl_index=[&](uint8_t data,int mode)->int{
            uint8_t l,r;
            if(!make_speed_lr(data,mode,l,r)) return 0;
            for(size_t e=0;e<entries.size();e++) if(entries[e].first==l&&entries[e].second==r) return (int)e+1;
            if(entries.size()>=gt::MAX_TABLELEN) return 0;
            entries.push_back({l,r});
            return (int)entries.size();
        };
        // Per-instrument: first column of the (vibdelay,rawvib) pair is the
        // direct vibdelay value (no transform), the second is the raw
        // vibrato-speed byte fed through makespeedtable (finevibrato mode).
        for(int i=0;i<N_INSTR;i++){
            legacy_vibdelay[i]=best.vibp[i];
            legacy_stblptr[i]=(uint8_t)stbl_index(best.vibd[i],MST_FINEVIB);
        }
        // Pattern commands: PORTAUP/PORTADOWN/TONEPORTA -> portamento mode,
        // VIBRATO -> finevibrato mode, FUNKTEMPO -> funktempo mode. All read
        // straight from the pattern's own arg byte.
        for(int p=0;p<=highest_patt&&p<gt::MAX_PATT;p++){
            for(int r=0;r<patt_len[p]&&r<gt::MAX_PATTROWS;r++){
                uint8_t cmd=patt_data[p][r*4+2];
                uint8_t &arg=patt_data[p][r*4+3];
                int mode=-1;
                if(cmd==gt::CMD_PORTAUP||cmd==gt::CMD_PORTADOWN||cmd==gt::CMD_TONEPORTA) mode=MST_PORTAMENTO;
                else if(cmd==gt::CMD_VIBRATO) mode=MST_FINEVIB;
                else if(cmd==gt::CMD_FUNKTEMPO) mode=MST_FUNKTEMPO;
                if(mode>=0) arg=(uint8_t)stbl_index(arg,mode);
            }
        }
        best.N_STBL=(int)entries.size();
        best.stbl_L.assign(entries.size(),0); best.stbl_R.assign(entries.size(),0);
        for(size_t e=0;e<entries.size();e++){best.stbl_L[e]=entries[e].first; best.stbl_R[e]=entries[e].second;}
        printf("Legacy STBL synthesized: N=%d\n",best.N_STBL);
    }

    bool nopulse=best.nopulse, nofilter=best.nofilter, noinsvib=best.noinsvib, fixedparams=best.fixedparams;
    (void)nopulse; (void)nofilter; (void)noinsvib;
    int N_WTBL=best.N_WTBL, N_PTBL=best.N_PTBL, N_FTBL=best.N_FTBL, N_STBL=best.N_STBL;

    // nowavedelay detected from disassembly in the wave_entry scan below
    bool nowavedelay=true;

    // ── Detect WTBL R-byte relative/absolute test polarity from the player ────
    // The wave-exec routine reads the current row's R-byte and branches on its
    // sign to decide relative (add to current note) vs absolute (use as-is)
    // pitch-offset interpretation. GoatTracker v2.0's standard players use
    // "bmi" (bit7 set = absolute, clear = relative) and store the R-byte
    // verbatim with no transform needed. From some point before v2.18 onward
    // (confirmed in v2.34/2.51/2.73 sources) this flipped to "bpl" (bit7 set =
    // relative, clear = absolute) -- the opposite polarity. Because both
    // conventions finish with "and #$7f", XOR-ing the raw byte with 0x80
    // exactly translates between them (R^0x80 leaves (N+R)&0x7f invariant for
    // any N, since 0x80 == 0 mod 128), so detecting which one this specific
    // binary's player actually uses is what decides whether to apply that XOR
    // -- not the legacy_format/STBL-presence flag, which conflates both eras.
    bool wtbl_r_needs_xor=false; // default: no transform (matches v2.0/legacy-format default)
    {
        int mt_wavetbl_final=icols0+best.ncols_try*N_INSTR+1;
        // Anchor on the wave-exec entry (the delay-threshold check we already
        // know the shape of: "lda wavetbl-1,y; cmp #$08-or-$10; bcs ...") and
        // search forward within that routine for the relative/absolute mask
        // ("and #$7f"), then look backward from there for the nearest bpl/bmi.
        // This is robust to the R-byte test not being textually adjacent to
        // its own table read (e.g. when an intervening "is R==0" special case
        // branches elsewhere first, as seen in this exact file).
        int wave_entry=-1;
        for(int i=0;i<code_len-5;i++){
            if(code[i]!=0xb9) continue;
            int a=code[i+1]|(code[i+2]<<8);
            if(a!=mt_wavetbl_final-1) continue;
            // NOWAVEDELAY==0 build: "cmp #$08-or-$10; bcs ...". NOWAVEDELAY==1
            // build (no delay feature compiled in): "beq mt_nowavechange"
            // directly, no cmp at all.
            if(code[i+3]==0xc9&&(code[i+4]==0x08||code[i+4]==0x10)){
                wave_entry=i;
                if(code[i+4]==0x10) nowavedelay=false; // v2.2+ CMP #$10: delay rows present
                break;
            }
            if(code[i+3]==0xf0){ wave_entry=i; break; } // BEQ: NOWAVEDELAY=1 → nowavedelay stays true
        }
        int and7f=-1, found_branch=0; // 0=not found, 1=bpl, -1=bmi
        if(wave_entry>=0){
            for(int j=wave_entry;j<wave_entry+200&&j<code_len-1;j++)
                if(code[j]==0x29&&code[j+1]==0x7f){ and7f=j; break; }
            if(and7f>=0){
                for(int k=and7f-1;k>=and7f-12&&k>=wave_entry;k--){
                    if(code[k]==0x10){ wtbl_r_needs_xor=true; found_branch=1; break; }
                    if(code[k]==0x30){ wtbl_r_needs_xor=false; found_branch=-1; break; }
                }
            }
        }
        printf("WTBL R-byte polarity: %s\n",
               found_branch>0?"bpl/inverted (xor applied)":
               found_branch<0?"bmi/standard (verbatim)":
               "not found in player code (defaulting to verbatim)");
        printf("nowavedelay=%d\n",(int)nowavedelay);
    }

    // ── fixedparams: scan player for GATETIMERPARAM and FIRSTWAVEPARAM ───────
    uint8_t fp_gate=6, fp_first=0x09;
    if(fixedparams){
        std::map<uint8_t,int> gc,fc;
        for(int pi=0;pi<code_len-3;pi++){
            if(code[pi]==0xc9&&(code[pi+2]==0xf0||code[pi+2]==0xd0)){uint8_t v=code[pi+1];if(v>0&&v<=0x3f)gc[v]++;}
            if(code[pi]==0xa9&&code[pi+2]==0x9d){uint8_t v=code[pi+1];if(v>=0x08)fc[v]++;}
        }
        if(!gc.empty()) fp_gate=gc.begin()->first;
        if(!fc.empty()) fp_first=fc.begin()->first;
        printf("fixedparams: gate=%02x firstwave=%02x\n",fp_gate,fp_first);
    }

    // ── Detect SIMPLEPULSE=1 from player code ─────────────────────────────────
    // SIMPLEPULSE=1 writes the same packed byte to both $D402,X and $D403,X
    // with no intervening LDA. Pattern: STA $D402,X (9D 02 D4) immediately
    // followed by STA $D403,X (9D 03 D4). This occurs both in the pulse player
    // loop and in voice-init sequences in SIMPLEPULSE=1 players; SIMPLEPULSE=0
    // players never have this adjacency since they write different hi/lo values.
    bool simplepulse = false;
    {
        static const uint8_t sp1[] = {0x9D,0x02,0xD4,0x9D,0x03,0xD4};
        for(int i=0;i<code_len-5;i++)
            if(memcmp(&code[i],sp1,6)==0){ simplepulse=true; break; }
        printf("simplepulse=%d\n",(int)simplepulse);
    }

    // ── Apply inverse transforms ────────────────────────────────────────────────
    // wtbl_l_inv's nowavedelay-shift question and the WTBL R-byte XOR question
    // are decided independently (L-byte delay/waveform remap vs R-byte pitch-
    // offset polarity); legacy_format alone is too coarse a signal for either,
    // but particularly for R, where we now detect the real player convention
    // directly above instead of assuming it from STBL presence.
    std::vector<uint8_t> wtbl_l(N_WTBL),wtbl_r(N_WTBL);
    for(int k=0;k<N_WTBL;k++){
        wtbl_l[k]=best.legacy_format?best.wtbl_L[k]:wtbl_l_inv(best.wtbl_L[k],nowavedelay);
        wtbl_r[k]=wtbl_r_needs_xor?wtbl_r_inv(best.wtbl_R[k],best.wtbl_L[k]):best.wtbl_R[k];
        if(getenv("WTBL_DEBUG")) printf("k=%2d rawL=%02x rawR=%02x -> outL=%02x outR=%02x\n",k,best.wtbl_L[k],best.wtbl_R[k],wtbl_l[k],wtbl_r[k]);
    }
    std::vector<uint8_t> ptbl_l(N_PTBL),ptbl_r(N_PTBL);
    for(int k=0;k<N_PTBL;k++){
        uint8_t bl=best.ptbl_L[k],br=best.ptbl_R[k];
        if(simplepulse && bl==0x80){
            // SIMPLEPULSE=1 SET entry: binary L is always clipped to 0x80,
            // binary R = (editor_L & 0x0F) | (editor_R & 0xF0). Invert:
            ptbl_l[k]=0x80|(br&0x0f);
            ptbl_r[k]=br&0xf0;
        } else {
            ptbl_l[k]=bl; ptbl_r[k]=br;
        }
    }
    std::vector<uint8_t> ftbl_l(N_FTBL),ftbl_r(N_FTBL);
    for(int k=0;k<N_FTBL;k++){
        ftbl_l[k]=best.legacy_format?best.ftbl_L[k]:ftbl_l_inv(best.ftbl_L[k]);
        ftbl_r[k]=best.ftbl_R[k];
    }
    std::vector<uint8_t> &stbl_l=best.stbl_L, &stbl_r=best.stbl_R;

    // ── Build SNG ─────────────────────────────────────────────────────────────
    gt::Song song; memset(&song,0,sizeof(song));
    for(int d=0;d<gt::MAX_SONGS;d++) for(int c=0;c<gt::MAX_CHN;c++){song.songorder[d][c][0]=gt::LOOPSONG;song.songorder[d][c][1]=0;}
    for(int p=0;p<gt::MAX_PATT;p++) song.pattern[p][0]=gt::ENDPATT;

    for(int i=0;i<32;i++){song.songname[i]    =(0x16+i<ds)?sid[0x16+i]:0;}
    for(int i=0;i<32;i++){song.authorname[i]  =(0x36+i<ds)?sid[0x36+i]:0;}
    for(int i=0;i<32;i++){song.copyrightname[i]=(0x56+i<ds)?sid[0x56+i]:0;}

    for(int k=0;k<N_WTBL;k++){song.ltable[gt::WTBL][k]=wtbl_l[k];song.rtable[gt::WTBL][k]=wtbl_r[k];}
    for(int k=0;k<N_PTBL;k++){song.ltable[gt::PTBL][k]=ptbl_l[k];song.rtable[gt::PTBL][k]=ptbl_r[k];}
    for(int k=0;k<N_FTBL;k++){song.ltable[gt::FTBL][k]=ftbl_l[k];song.rtable[gt::FTBL][k]=ftbl_r[k];}
    for(int k=0;k<N_STBL;k++){song.ltable[gt::STBL][k]=stbl_l[k];song.rtable[gt::STBL][k]=stbl_r[k];}

    // ── Patterns (already unpacked above, independent of table addresses) ─────
    for(int p=0;p<=highest_patt&&p<gt::MAX_PATT;p++){
        if(patt_len[p]<=0) continue;
        memcpy(song.pattern[p],patt_data[p].data(),(gt::MAX_PATTROWS*4+4));
        song.pattlen[p]=patt_len[p];
        song.highestusedpattern=p;
    }

    // ── Instruments ───────────────────────────────────────────────────────────
    song.highestusedinstr=N_INSTR;
    for(int i=0;i<N_INSTR;i++){
        int idx=i+1;
        int fp=best.filtpt[i]; if(fp<1||fp>N_FTBL) fp=0;
        int pp=best.pptr[i];   if(pp<1||pp>N_PTBL) pp=0;
        uint8_t vd; int vp;
        if(best.legacy_format){
            vd=legacy_vibdelay[i];
            vp=legacy_stblptr[i];
        } else {
            vp=best.vibp[i];   if(vp<1||vp>N_STBL) vp=0;
            // GT2.73's relocator writes vibdelay-1 to the compiled SID
            // (greloc.c: "instrwork[...]=instr[c].vibdelay-1" when nonzero),
            // so the .sng/editor value is the compiled byte plus one.
            vd=(best.vibd[i]>0)?(uint8_t)(best.vibd[i]+1):0;
            if(vp>0&&vd==0) vd=1;
        }
        song.instr[idx].ad        =adc[i];
        song.instr[idx].sr        =srv[i];
        song.instr[idx].gatetimer =fixedparams?fp_gate:best.gatec[i];
        song.instr[idx].firstwave =fixedparams?fp_first:best.firstc[i];
        song.instr[idx].vibdelay  =vd;
        song.instr[idx].ptr[gt::STBL]=(uint8_t)vp;
        song.instr[idx].ptr[gt::FTBL]=(uint8_t)fp;
        song.instr[idx].ptr[gt::WTBL]=wptr[i];
        song.instr[idx].ptr[gt::PTBL]=(uint8_t)pp;
    }

    // ── Orderlists ────────────────────────────────────────────────────────────
    // Binary: [pattern, REPEAT_byte] → SNG: [REPEAT_byte, pattern]
    printf("Patt table: %d patterns, lo=SID %04x hi=SID %04x\n",n_patts_v,mt_patttbllo,mt_patttblhi);
    printf("n_songs=%d  n_patts=%d\n",n_songs_v,n_patts_v);
    for(int sg=0;sg<n_songs_v&&sg<gt::MAX_SONGS;sg++){
        for(int ch=0;ch<3;ch++){
            int lo=sid[s2f(mt_songtbllo+sg*3+ch)],hi=sid[s2f(mt_songtblhi+sg*3+ch)];
            int f=s2f((hi<<8)|lo);
            std::vector<uint8_t> raw;
            int fp=f;
            while(fp>=0&&fp<flen&&sid[fp]!=0xFF&&(int)raw.size()<gt::MAX_SONGLEN) raw.push_back(sid[fp++]);
            uint8_t rst=(fp+1<flen)?sid[fp+1]:0;
            int d=0;
            for(int i=0;i<(int)raw.size()&&d<gt::MAX_SONGLEN;){
                uint8_t b=raw[i],nxt=(i+1<(int)raw.size())?raw[i+1]:0;
                if(b<gt::REPEAT&&nxt>gt::REPEAT&&nxt<=gt::REPEAT+15){
                    song.songorder[sg][ch][d++]=nxt; song.songorder[sg][ch][d++]=b; i+=2;
                } else { song.songorder[sg][ch][d++]=b; i++; }
            }
            song.songorder[sg][ch][d]=gt::LOOPSONG; song.songorder[sg][ch][d+1]=rst;
            song.songlen[sg][ch]=d;
        }
    }

    if(!song.save(argv[2])){fprintf(stderr,"Save failed\n");return 1;}
    printf("OK: %s  %d songs  %d instrs  %d patts  WTBL=%d PTBL=%d FTBL=%d STBL=%d\n",
           argv[2],n_songs_v,N_INSTR,n_patts_v,N_WTBL,N_PTBL,N_FTBL,N_STBL);
    return 0;
}
