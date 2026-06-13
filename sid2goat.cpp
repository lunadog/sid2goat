// sid2goat — GoatTracker 2 / GTUltra SID → SNG converter
// Inverts greloc.c binary format back to editable GoatTracker source.
#include "gsong.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <map>
#include <set>

// ── Pattern decode (inverse of greloc.c packpattern) ────────────────────────
// Binary layout: 0x00=END, 0x01-0x3F=instr-change, 0x40-0x4F=note+FX,
//   0x50-0x5F=REST+FX, 0x60-0xBD=note/REST, 0xC0-0xFE=packed-rest.
// CMD_SETTEMPO: binary arg = sng_arg-1 (for sng_arg≥3) → inverse: +1 if ≥2.
static int unpack_patt(uint8_t *out, const uint8_t *data, int off, int maxlen) {
    int pos=off, row=0;
    uint8_t cur_instr=0, cmd=0, arg=0;
    while (row<gt::MAX_PATTROWS && pos<maxlen) {
        uint8_t b=data[pos++];
        if (b==0x00) break;
        uint8_t row_instr=0;
        if (b<gt::FX) {
            cur_instr=b; row_instr=b;
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
            uint8_t oi=(b!=gt::REST)?(row_instr?row_instr:cur_instr):0;
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
        int n=2;
        while(s+n<(int)da.size()&&da[s+n].first-da[s+n-1].first==g) n++;
        int cmin=da[s].second,cmax=da[s].second;
        for(int k=1;k<n;k++){cmin=std::min(cmin,da[s+k].second);cmax=std::max(cmax,da[s+k].second);}
        if(cmax-cmin<=300&&n>best_n){best_s=s;best_g=g;best_n=n;}
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

    // ── Locate WTBL rtable (mt_notetbl) ──────────────────────────────────────
    int mt_notetbl=0;
    for(auto&[sa,co]:da){
        if(sa<=mt_wavetbl) continue;
        if((aset.count(sa-1)&&sa-1>mt_wavetbl)||(aset.count(sa+1)&&sa+1>mt_wavetbl)){
            mt_notetbl=std::max(sa,(aset.count(sa+1)?sa+1:sa))+1; break;
        }
    }
    if(!mt_notetbl){int mx=1;if(N_COLS>2)for(int i=0;i<N_INSTR;i++)mx=std::max(mx,(int)sid[s2f(icols[2]+1+i)]);mt_notetbl=mt_wavetbl+mx+4;}
    int N_WTBL=mt_notetbl-mt_wavetbl;
    printf("N_WTBL=%d  mt_notetbl=SID %04x\n",N_WTBL,mt_notetbl);

    // ── Locate PTBL ──────────────────────────────────────────────────────────
    int wtbl_r_end=mt_notetbl+N_WTBL;
    int mt_ptbl=wtbl_r_end;
    if(!aset.count(wtbl_r_end-1)) for(int d=-5;d<=5;d++) if(aset.count(wtbl_r_end+d-1)){mt_ptbl=wtbl_r_end+d;break;}
    printf("mt_ptbl=SID %04x\n",mt_ptbl);
    int mt_pulsespd=0;
    for(auto&[sa,cnt]:acnt) if(sa>mt_ptbl&&cnt>=2){mt_pulsespd=sa+1;break;}
    if(!mt_pulsespd) mt_pulsespd=mt_ptbl+N_INSTR*3;
    int N_PTBL=mt_pulsespd-mt_ptbl;
    printf("N_PTBL=%d  mt_pulsespd=SID %04x\n",N_PTBL,mt_pulsespd);

    // ── Detect nowavedelay ────────────────────────────────────────────────────
    bool nowavedelay=true;
    { bool dir=false,shft=false;
      for(int k=0;k<N_WTBL;k++){uint8_t v=sid[s2f(mt_wavetbl+k)];if(v==0x41||v==0x81)dir=true;if(v==0x51||v==0x91)shft=true;}
      if(!dir&&shft) nowavedelay=false; }
    printf("nowavedelay=%d\n",(int)nowavedelay);

    // ── Derive column flags from N_COLS ───────────────────────────────────────
    // Layout: AD,SR,WPTR,[PPTR],[ FPTR],[SPTR,VDLY],[GATE,FW]
    bool nopulse,nofilter,noinsvib,fixedparams;
    nopulse=(N_PTBL==0);
    { int rem=N_COLS-3-(nopulse?0:1);
      nofilter=(rem<5); if(!nofilter) rem--;
      noinsvib=(rem<2); fixedparams=(noinsvib?(rem<2):(rem<4)); }

    auto read_col=[&](int ci)->std::vector<uint8_t>{
        std::vector<uint8_t> v(N_INSTR,0);
        if(ci<N_COLS) for(int i=0;i<N_INSTR;i++) v[i]=sid[s2f(icols[ci]+1+i)];
        return v;
    };
    int ci=0;
    auto adc=read_col(ci++),sr=read_col(ci++),wptr=read_col(ci++);
    auto pptr  =nopulse  ?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
    auto filtpt=nofilter ?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
    auto vibp  =noinsvib ?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
    auto vibd  =noinsvib ?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
    auto gatec =fixedparams?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
    auto firstc=fixedparams?std::vector<uint8_t>(N_INSTR,0):read_col(ci);

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

    // ── Read WTBL and PTBL ────────────────────────────────────────────────────
    std::vector<uint8_t> wtbl_l(N_WTBL),wtbl_r(N_WTBL),ptbl_l(N_PTBL),ptbl_r(N_PTBL);
    for(int k=0;k<N_WTBL;k++){uint8_t lb=sid[s2f(mt_wavetbl+k)],rb=sid[s2f(mt_notetbl+k)];wtbl_l[k]=wtbl_l_inv(lb,nowavedelay);wtbl_r[k]=wtbl_r_inv(rb,lb);}
    for(int k=0;k<N_PTBL;k++){ptbl_l[k]=sid[s2f(mt_ptbl+k)];ptbl_r[k]=sid[s2f(mt_pulsespd+k)];}

    // ── Find song and pattern tables from player code ─────────────────────────
    // Signature: LDA tbl_lo,Y / STA zp / LDA tbl_hi,Y / STA zp
    // Orderlist variant has LDA (zp),Y / CMP #$FF after; pattern variant doesn't.
    int mt_songtbllo=-1,mt_songtblhi=-1,mt_patttbllo=-1,mt_patttblhi=-1;
    for(int i=0;i<code_len-20&&(mt_songtbllo<0||mt_patttbllo<0);i++){
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
            if(has_ol&&mt_songtbllo<0){mt_songtbllo=a1;mt_songtblhi=a2;}
            else if(!has_ol&&mt_patttbllo<0){mt_patttbllo=a1;mt_patttblhi=a2;}
            break;
        }
    }
    if(mt_songtbllo<0||mt_patttbllo<0){fprintf(stderr,"Could not find song/pattern tables in 6502 code\n");return 1;}

    int n_songs_v=(mt_songtblhi-mt_songtbllo)/3;
    int n_patts_v=mt_patttblhi-mt_patttbllo;
    int mt_insad_v=mt_patttblhi+n_patts_v;

    // Refine instrument cluster using definitive mt_insad
    {
        std::vector<int> ic2;
        for(auto&[sa,co]:cl) if(sa>=mt_insad_v-1) ic2.push_back(sa);
        if(ic2.size()>=2){
            icols=ic2; N_COLS=(int)icols.size()-1; N_INSTR=icols[1]-icols[0];
            mt_wavetbl=icols.back()+1;
            nopulse=(N_PTBL==0);
            {int rem=N_COLS-3-(nopulse?0:1);nofilter=(rem<5);if(!nofilter)rem--;noinsvib=(rem<2);fixedparams=(noinsvib?(rem<2):(rem<4));}
            ci=0;
            adc=read_col(ci++); sr=read_col(ci++); wptr=read_col(ci++);
            pptr  =nopulse  ?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
            filtpt=nofilter ?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
            vibp  =noinsvib ?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
            vibd  =noinsvib ?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
            gatec =fixedparams?std::vector<uint8_t>(N_INSTR,0):read_col(ci++);
            firstc=fixedparams?std::vector<uint8_t>(N_INSTR,0):read_col(ci);
            printf("N_INSTR=%d  N_COLS=%d  mt_wavetbl=SID %04x (refined)\n",N_INSTR,N_COLS,mt_wavetbl);
        }
    }

    int ol_file=flen;
    for(int c=0;c<n_songs_v*3;c++){int lo=sid[s2f(mt_songtbllo+c)],hi=sid[s2f(mt_songtblhi+c)];int af=s2f((hi<<8)|lo);if(af>=ds&&af<flen)ol_file=std::min(ol_file,af);}
    if(ol_file>=flen){fprintf(stderr,"Bad song table addresses\n");return 1;}
    printf("Orderlists at file %04x\n",ol_file);
    int ol_SID=f2s(ol_file);
    int ptbl_end_SID=mt_pulsespd+N_PTBL;

    // ── Locate FTBL and STBL from LDA abs,Y accesses in the table gap ────────
    // greloc layout: FTBL_L | FTBL_R | [0x00] STBL_L [0x00] STBL_R
    // All four table bases appear as LDA abs,Y in the player; their base-1
    // addresses land in the gap [ptbl_end_SID, ol_SID).
    // N_FTBL = a2-a1+1  (a1=FTBL_L base, a2=FTBL_R base-1, inclusive span)
    // N_STBL = a4-a3-1  (a3=STBL_L base-1 / separator, a4=STBL_R base-1)
    int N_FTBL=0,N_STBL=0;
    int mt_ftbl=0,mt_ftblspd=0,mt_speedlefttbl=0,mt_speedrighttbl=0;
    {
        std::vector<int> gap;
        for(auto&[sa,cnt]:acnt) if(sa>=ptbl_end_SID&&sa<ol_SID) gap.push_back(sa);
        std::sort(gap.begin(),gap.end());
        if(gap.size()>=4){
            int a1=gap[0],a2=gap[1],a3=gap[gap.size()-2],a4=gap.back();
            N_FTBL=a2-a1+1; mt_ftbl=a1; mt_ftblspd=a2+1;
            N_STBL=a4-a3-1; mt_speedlefttbl=a3+1; mt_speedrighttbl=a4+1;
        } else if(gap.size()==2){
            int a3=gap[0],a4=gap[1];
            N_STBL=a4-a3-1; mt_speedlefttbl=a3+1; mt_speedrighttbl=a4+1;
        } else if(gap.size()==1){
            mt_speedrighttbl=gap[0]+1; N_STBL=ol_SID-mt_speedrighttbl;
        }
        printf("FTBL: N=%d ltable=SID %04x rtable=SID %04x\n",N_FTBL,mt_ftbl,mt_ftblspd);
        printf("STBL: N=%d ltable=SID %04x rtable=SID %04x\n",N_STBL,mt_speedlefttbl,mt_speedrighttbl);

        if(N_FTBL==0) nofilter=true;
        if(N_STBL==0) noinsvib=true;
        // nofilter may have been set conservatively (N_COLS<8) even when a filter
        // table exists. Re-read the filter pointer column now that N_FTBL is known.
        if(nofilter&&N_FTBL>0&&!icols.empty()){
            nofilter=false;
            int fc=3+(nopulse?0:1);
            if(fc<(int)icols.size()) for(int i=0;i<N_INSTR;i++) filtpt[i]=sid[s2f(icols[fc]+1+i)];
        }
        if(nofilter)  for(int i=0;i<N_INSTR;i++) filtpt[i]=0;
        if(noinsvib){ for(int i=0;i<N_INSTR;i++) vibp[i]=vibd[i]=0; }
    }

    // ── Build SNG ─────────────────────────────────────────────────────────────
    gt::Song song; memset(&song,0,sizeof(song));
    for(int d=0;d<gt::MAX_SONGS;d++) for(int c=0;c<gt::MAX_CHN;c++){song.songorder[d][c][0]=gt::LOOPSONG;song.songorder[d][c][1]=0;}
    for(int p=0;p<gt::MAX_PATT;p++) song.pattern[p][0]=gt::ENDPATT;

    for(int i=0;i<32;i++){song.songname[i]    =(0x16+i<ds)?sid[0x16+i]:0;}
    for(int i=0;i<32;i++){song.authorname[i]  =(0x36+i<ds)?sid[0x36+i]:0;}
    for(int i=0;i<32;i++){song.copyrightname[i]=(0x56+i<ds)?sid[0x56+i]:0;}

    // WTBL and PTBL: direct copy after inverse transforms
    for(int k=0;k<N_WTBL;k++){song.ltable[gt::WTBL][k]=wtbl_l[k];song.rtable[gt::WTBL][k]=wtbl_r[k];}
    for(int k=0;k<N_PTBL;k++){song.ltable[gt::PTBL][k]=ptbl_l[k];song.rtable[gt::PTBL][k]=ptbl_r[k];}

    // FTBL: all N_FTBL entries stored verbatim from the binary.
    // L: inverse passband transform for mode bytes (>0x80, ≠0xFF).
    // R: verbatim (stop=0x00, loop targets, routing bytes all kept as-is).
    // Fptrs and CMD args also stored verbatim (binary compact indices).
    // This produces an identity tablemap when GTUltra recompiles: all entries
    // are seeded via the existing fptrs/CMDs, so the output binary is identical.
    if(N_FTBL>0&&mt_ftbl>0){
        for(int k=1;k<=N_FTBL&&k<=gt::MAX_TABLELEN;k++){
            uint8_t bl=sid[s2f(mt_ftbl+k-1)], br=sid[s2f(mt_ftblspd+k-1)];
            song.ltable[gt::FTBL][k-1]=ftbl_l_inv(bl);
            song.rtable[gt::FTBL][k-1]=br;
        }
        for(int i=0;i<N_INSTR;i++){int fp=filtpt[i];if(fp<1||fp>N_FTBL)filtpt[i]=0;}

        for(int p=0;p<n_patts_v&&p<gt::MAX_PATT;p++){
            int lo=sid[s2f(mt_patttbllo+p)],hi=sid[s2f(mt_patttblhi+p)];
            int pf=s2f((hi<<8)|lo);
            if(pf<ds||pf>=flen){song.pattlen[p]=0;continue;}
            int rows=unpack_patt(song.pattern[p],sid,pf,flen);
            song.pattlen[p]=rows; song.highestusedpattern=p;
        }
    }

    // STBL: direct copy (no transform)
    if(N_STBL>0&&mt_speedrighttbl>0){
        int stbl_l=mt_speedrighttbl-N_STBL-1;
        for(int k=0;k<N_STBL&&k<gt::MAX_TABLELEN;k++){
            song.ltable[gt::STBL][k]=sid[s2f(stbl_l+k)];
            song.rtable[gt::STBL][k]=sid[s2f(mt_speedrighttbl+k)];
        }
    }

    // Patterns for songs with no filter table
    if(N_FTBL==0){
        for(int p=0;p<n_patts_v&&p<gt::MAX_PATT;p++){
            int lo=sid[s2f(mt_patttbllo+p)],hi=sid[s2f(mt_patttblhi+p)];
            int pf=s2f((hi<<8)|lo);
            if(pf<ds||pf>=flen){song.pattlen[p]=0;continue;}
            int rows=unpack_patt(song.pattern[p],sid,pf,flen);
            song.pattlen[p]=rows; song.highestusedpattern=p;
        }
    }

    // ── Instruments ───────────────────────────────────────────────────────────
    song.highestusedinstr=N_INSTR;
    for(int i=0;i<N_INSTR;i++){
        int idx=i+1;
        uint8_t vd=(vibd[i]>0)?(uint8_t)(vibd[i]+1):0;
        if(vibp[i]>0&&vd==0) vd=1;
        song.instr[idx].ad        =adc[i];
        song.instr[idx].sr        =sr[i];
        song.instr[idx].gatetimer =fixedparams?fp_gate:gatec[i];
        song.instr[idx].firstwave =fixedparams?fp_first:firstc[i];
        song.instr[idx].vibdelay  =vd;
        song.instr[idx].ptr[gt::STBL]=vibp[i];
        song.instr[idx].ptr[gt::FTBL]=filtpt[i];
        song.instr[idx].ptr[gt::WTBL]=wptr[i];
        song.instr[idx].ptr[gt::PTBL]=pptr[i];
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
