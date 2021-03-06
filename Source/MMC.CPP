//functions for implementation of memory mappers
#include <stdlib.h>
#include <string.h>


#include "message.h"

#include "nes.h"
#include "nesvideo.h"
#include "ppu.h"
#include "mmc.h"


#include "m6502.h"

#include "r2img.h"
#include "font.h"
#include "dd.h"

#include "keyb.h"

#include "timing.h"

#include "slist.h"

#define FREE(x) if (x) {free(x); x=0;}
#define DELETE(x) if (x) {delete x; x=0;}

extern int blah,blah2;


//set ROM at 0x8000 to *rom
void setrombank(ROMBANK *rom)
{
 m6502rom=(byte *)(rom-1); //offset by 0x8000
}

void ROMBANK::setlowpage(ROMPAGE *p)
{
 if (!p) memset(s+0,0,sizeof(ROMPAGE)); //clear it
    else memcpy(s+0,p,sizeof(ROMPAGE)); //set it
}

void ROMBANK::sethighpage(ROMPAGE *p)
{
 if (!p) memset(s+0x4000,0,sizeof(ROMPAGE)); //clear it
    else memcpy(s+0x4000,p,sizeof(ROMPAGE)); //set it
}

inline byte NES_ppumemory::read(word a)
{
 if (a>=0x2000 && a<0x3000) //name table wants to be read
    return nv->ntc[(a-0x2000)/0x400]->read(a&0x3FF);
 return ((byte *)this)[a];
}



//------------------------------------------
//no memory mapper

class mmc_none:public memorymapper
{
 ROMBANK r; //only one rombank

 public:

 mmc_none()
 {
  //set default timing
  HBLANKCYCLES=100;
  VBLANKLINES=33;
 }
 //read from VRAM like normal
 byte ppuread(word a) {return ppu->read(a);}

 virtual void reset()
 {
  //initialize rombank
  r.setlowpage(&ROM[0]);
  r.sethighpage(&ROM[numrom-1]);

  //set it!
  setrombank(&r);

   //copy vrom to ppu address space (if it exists)
  if (numvrom>0) memcpy(ppu,VROM,sizeof(VROMPAGE));
 }
            /*
 virtual int write(word a,byte d)
 {
  msg.printf(2,"write[%X]=%X",a,d);
  return 0;
 }

 virtual int write5000(word a,byte d)
 {
  msg.printf(2,"write[%X]=%X",a,d);
  return 0;
 }*/

};





//------------------------------------------
//Konami memory mapper

class mmc_konami:public memorymapper
{
 ROMBANK *r; //list of all possible rom bank combinations

 byte rombank; //last 16k rom bank set

 public:
 mmc_konami()
  {
   HBLANKCYCLES=115;
   VBLANKLINES=24;
   r=(ROMBANK *)malloc(numrom*sizeof(ROMBANK));
  } //create all rom banks
 ~mmc_konami() {free(r);}

 void save(char *t) {t[0]=rombank;}
 void restore(char *t) {rombank=t[0]; setrombank(&r[rombank]);}

 void reset()
 {
  //copy rom into rom banks
  for (int i=0; i<numrom; i++)
   {
    r[i].setlowpage(&ROM[i]);
    r[i].sethighpage(&ROM[numrom-1]);
   }
  setrombank(&r[0]);

  rombank=0;
//  msg.printf(1,"Konami mapper reset");
 }

  //write to ROM area
 int write(word a,byte d)
  {
//msg.error("KONAMI[%X]=%X",a,d);
   //switch rom banks
   if (d<numrom)
    {
     setrombank(&r[d]);
     rombank=d;
//     memcpy(ram+0x8000,&ROM[rombank],sizeof(ROMPAGE));
     return 1;
    }
   return 0;
  }

 virtual byte getbank() {return rombank;}

 byte ppuread(word a) {return ppu->read(a);}
};


//------------------------------------------
//VROMswitch memory mapper

int getscanline();

class mmc_vromswitch:public mmc_none
{
 char vrombank; //current 8K vrom bank

 public:
 mmc_vromswitch()
  {
   HBLANKCYCLES=115;
   VBLANKLINES=33;
  }

 void save(char *t) {t[0]=vrombank;}
 void restore(char *t) {nv->sl.addevent(SE_VROM8K,vrombank=t[0]); }

 //reset mapper
 void reset()
 {
  mmc_none::reset();
  vrombank=0;
  nv->sl.addevent(SE_VROM8K,vrombank);
//  msg.printf(2,"VROMswitch reset");
 }
 //write
 virtual int write(word a,byte d)
 {
  if (numvrom)
   {
    vrombank=d%numvrom; //store vrombank
    nv->sl.addevent(SE_VROM8K,vrombank);
   }
//  msg.printf(2,"vrombank[%X]=%X line=%d",a,d%numvrom,getscanline());
  return 0;
 }

 //read from VROM bank selected
 byte ppuread(word a)
  {
   if (a>=0x2000) return ppu->read(a);
   if (!numvrom) return ppu->read(a);
   return VROM[vrombank][a]; //read from VROM
  }
};


//------------------------------------------
//Sequential memory mapper

class mmc_sequential:public memorymapper
{
 ROMBANK *r; //list of all possible rom bank combinations
 byte reg[4]; //4 registers

 byte numvrom8k; //number of 8k vrom pages
 byte numvrom4k; //number of 4k vrom pages

 char bit;    //next bit expected
 byte tempreg; //temp reg being read

 public:
 mmc_sequential()
  {
   HBLANKCYCLES=123; //115;
   VBLANKLINES=33;
   r=(ROMBANK *)malloc(numrom*sizeof(ROMBANK));
  }
 ~mmc_sequential() {free(r);}

 virtual byte getbank() {return reg[3];}

 void reset()
 {
  //copy rom into rom banks
  for (int i=0; i<numrom; i++)
   {
    r[i].setlowpage(&ROM[i]);
    r[i].sethighpage(&ROM[numrom-1]);
   }
  setrombank(&r[0]);

  numvrom8k=numvrom; numvrom4k=numvrom*2;

  bit=0; tempreg=0;
  reg[0]=reg[1]=reg[2]=reg[3]=0;
//  msg.printf(2,"Sequential mapper reset");
 }

 void updatereg(int num,byte d)
 {
//     msg.printf(2,"seqreg[%X]=%X",num,d);
  switch(num)
  {
   case 0: //Mirroring / vrom page select
     if ((d&0x10)!=(reg[0]&0x10))
      {
       reg[0]=d;   updatereg(1,0); updatereg(2,0);
      } else reg[0]=d;
     nv->setmirroring((reg[0]&1) ? HMIRROR : VMIRROR);
    break;
   case 1: //VROM page select
    if (!numvrom) break;
    if (reg[0]&0x10) //vrom4k
     {
      reg[1]=d%numvrom4k; //(d<numvrom4k ? d : 0);
      nv->sl.addevent(SE_VROM4K0,reg[1]);
     } else
     {
      reg[1]=d%numvrom8k; //(d<numvrom8k ? d : 0);
      nv->sl.addevent(SE_VROM8K,reg[1]);
     }
    break;
   case 2: //second VROM page select
    if (!numvrom) break;
    if (reg[0]&0x10) //vrom4k
     {
      reg[2]=d%numvrom4k; //(d<numvrom4k ? d : 0);
      nv->sl.addevent(SE_VROM4K1,reg[2]);
     }
    break;
   case 3: //ROM PAGE select
//     if (d==reg[3]) return;
    if (d<numrom) setrombank(&r[d]);
//     memcpy(ram+0x8000+0x0000,((char *)&ROM[d])+0x0000,0x8000);
     reg[3]=d;
    break;
  }
 }

 void save(char *t)
  {for (int i=0; i<4; i++)  t[i]=reg[i];}
 void restore(char *t)
  {for (int i=0; i<4; i++)  updatereg(i,t[i]);}

 virtual int write(word a,byte d)
 {
//  if (!bit) msg.printf(1,"seq[%X]=%X",a,d);
  if (d&0x80) {bit=0; tempreg=0; return 0; } //reset
  if (d&1) tempreg|=(1<<bit);
  bit++;
  if (bit>=5)
   {
    updatereg((a-0x8000)/0x2000,tempreg);
    bit=0; tempreg=0;
    return 1;
   }
  return 0;
 }

 //read from VROM bank selected
 byte ppuread(word a)
  {
   if (a>=0x2000) return ppu->read(a);
   //read from vram
   if (!numvrom) return ppu->read(a);

   //read from VROM
   if (!(reg[0]&0x10)) return reg[1]<numvrom8k ? VROM[reg[1]][a] : 0; //8k
    else
    {
     if (a<0x1000) return reg[1]<numvrom4k ? ((byte *)VROM)[reg[1]*0x1000+a] : 0;//first 4k rom
              else return reg[2]<numvrom4k ? ((byte *)VROM)[reg[2]*0x1000+a-0x1000] : 0;//first 4k rom
    }
  }

};


//----------------------------------------
// 4 8K bank memory mapper

//rom configuration
struct rompage8k
{
 byte page[4];
 int operator ==(rompage8k &t) {return *((unsigned *)this)==*((unsigned *)&t);}
};

//possible rom
struct ROM8K
{
 rompage8k rp;      //page numbers for data
 unsigned time;     //when was the last time this was used?

 byte rom[4][0x2000]; //actual data

 //create page
 void create(rompage8k _rp)
 {
  rp=_rp;
  time=uu;
  for (int i=0; i<4; i++)
   {
    if (rp.page[i]>=numrom*2) rp.page[i]=0; //ignore it
    memcpy(rom[i],((char *)ROM)+rp.page[i]*0x2000,0x2000); //copy page!
   }
//  msg.printf(1,"romcreate: %X %X %X %X",rp.page[0],rp.page[1],rp.page[2],rp.page[3]);
 }

  //set this rom bank as the current one
 void set()
  {
   time=uu;
   setrombank((ROMBANK *)&rom);
//  msg.printf(1,"%X %X %X %X",rp.page[0],rp.page[1],rp.page[2],rp.page[3]);
  }
};


#define NUMROMCACHE 16

class mapper8krom:public memorymapper
{
 protected:
 int numrom8k;  //number of 8k roms
 rompage8k rp; //rom page config
 ROM8K  *R[NUMROMCACHE]; //cache of rom pages

 public:
 mapper8krom():numrom8k(numrom*2)
  {memset(R,0,NUMROMCACHE*sizeof(ROM8K *));}
 ~mapper8krom() {for (int i=0; i<NUMROMCACHE; i++) DELETE(R[i]);}

 void setrompages()
 {
  int i;
  for (i=0; i<NUMROMCACHE; i++)
   if (R[i] && R[i]->rp==rp)//does one already exist?
      {R[i]->set(); return;}  //set it
  //one was not found...create one
  for (i=0; i<NUMROMCACHE; i++)
   if (!R[i]) {R[i]=new ROM8K; break;}
  //cache is full! find the least used one
  if (i==NUMROMCACHE)
   {
    unsigned mintime=0xFFFFFFFF;
    byte minpage=0;
    for (i=0; i<NUMROMCACHE; i++)
     if (R[i]->time<mintime) {mintime=R[i]->time; minpage=i;}
    i=minpage; //this is the page to use
//    msg.printf(3,"trash %d",i);
   }
  //fill page up
  R[i]->create(rp);
  R[i]->set();
 }


};


//------------------------------------------
//5202 memory mapper


extern byte IRQline,doIRQ;
class mmc_5202:public mapper8krom
{
 int numvrom1k; //number of 1k vroms
 byte vrompage[8]; //current 1k vrom pages set
 byte command; //last command read

 public:
 mmc_5202()
  {
   numvrom1k=numvrom*8;
   HBLANKCYCLES=150;
   VBLANKLINES=24;
  }

 void reset()
 {
  //set rom page stuff
  rp.page[0]=numrom8k-2;
  rp.page[1]=numrom8k-1;
  rp.page[2]=numrom8k-2;
  rp.page[3]=numrom8k-1;
  setrompages();

  //set all vrom page stuff
  for (int i=0; i<8; i++) {vrompage[i]=i;}

  command=0;
  //msg.printf(2,"5202 mapper reset");
 }

  //do command c with data d
 int docommand(byte c,byte d)
 {
  //if (c<6)  msg.printf(2,"command[%X]=%X",c,d);
  byte vs=(c&0x80) ? 4 : 0;
  switch (c&7)
   {
    case 0:
       d&=~1;
       nv->sl.addevent(SE_VROM1K+(vs^0),vrompage[vs^0]=d);
       nv->sl.addevent(SE_VROM1K+(vs^1),vrompage[vs^1]=(d|1));
       return 0;
    case 1:
       d&=~1;
       nv->sl.addevent(SE_VROM1K+(vs^2),vrompage[vs^2]=d);
       nv->sl.addevent(SE_VROM1K+(vs^3),vrompage[vs^3]=(d|1));
       return 0;
    case 2: vs^=4; vrompage[vs]=d; nv->sl.addevent(SE_VROM1K+vs,d); return 0;
    case 3: vs^=5; vrompage[vs]=d; nv->sl.addevent(SE_VROM1K+vs,d); return 0;
    case 4: vs^=6; vrompage[vs]=d; nv->sl.addevent(SE_VROM1K+vs,d); return 0;
    case 5: vs^=7; vrompage[vs]=d; nv->sl.addevent(SE_VROM1K+vs,d); return 0;
    case 6: rp.page[(c&0x40) ? 2 : 0]=d; setrompages(); return 1;
    case 7: rp.page[(c&0x40) ? 1 : 1]=d; setrompages(); return 1;
   }
  return 0;
 }

 virtual int write(word a,byte d)
 {
//  if ((a&0xFF00)!=0x8000) msg.printf(2,"5202[%X]=%X pc=%X",a,d,m6502pc);
  switch (a)
  {
   case 0xA000:
    nv->setmirroring((d&1) ? HMIRROR : VMIRROR);
//    msg.printf(2,"5202[%X]=%X pc=%X",a,d,m6502pc);
   return 0;

   case 0x8000: command=d; return 0;
   case 0x8001: return docommand(command,d);
   case 0xE000:
//    msg.printf(2,"5202[%X]=%X",a,d);
   return 0;

   case 0xE001:
    return 0;
   case 0xC001:
//    msg.printf(2,"5202[%X]=%X",a,d);
   case 0xC000:
//    msg.printf(2,"5202[%X]=%X",a,d);
    setIRQ(d); //+getscanline());
   return 0;

   default:  return 0;
  }
 }

 void save(char *t)
  {
   int i;
   for (i=0; i<4; i++) t[i]=rp.page[i];
   for (i=0; i<8; i++) t[i+4]=vrompage[i];
  }
 void restore(char *t)
  {
   int i;
   for (i=0; i<4; i++) rp.page[i]=t[i];
   for (i=0; i<8; i++)
    nv->sl.addevent(SE_VROM1K+i,vrompage[i]=t[i+4]);
   setrompages();
 }

 byte ppuread(word a)
 {
  if (a>=0x2000) return ppu->read(a);
  //read from vram
  if (!numvrom) return ppu->read(a);

  byte vp=vrompage[(a/0x400)];
  return ((char *)VROM)[(vp*0x400)+(a&0x3FF)];
 }

};







//------------------------------------------
//castlevania 3

class mmc_castlevania3:public memorymapper
{
 ROMBANK r; //only one rombank

 public:

 //read from VRAM like normal
 byte ppuread(word a) {return ppu->read(a);}

 virtual void reset()
 {
  //initialize rombank
  r.setlowpage(&ROM[numrom-2]);
  r.sethighpage(&ROM[numrom-1]);

  //set it!
  setrombank(&r);

   //copy vrom to ppu address space (if it exists)
  if (numvrom>0) memcpy(ppu,VROM,sizeof(VROMPAGE));
 }

 virtual int write(word a,byte d)
 {
  msg.printf(2,"write[%X]=%X",a,d);
  return 0;
 }

 virtual int write5000(word a,byte d)
 {
  msg.printf(2,"write[%X]=%X",a,d);
  return 0;
 }

};







//------------------------------------------
//  MMC_F4xxx

class mmc_f4xxx:public mapper8krom
{
 public:
 //read from VRAM like normal
 byte ppuread(word a) {return ppu->read(a);}

 void reset()
 {
  //set rom page stuff
  rp.page[0]=numrom8k-2;
  rp.page[1]=numrom8k-1;
  rp.page[2]=numrom8k-2;
  rp.page[3]=numrom8k-1;
  setrompages();

  //set all vrom page stuff
  if (numvrom>0) memcpy(ppu,VROM,sizeof(VROMPAGE));
//  msg.printf(1,"numrom8k=%X",numrom8k);
 }

 virtual int write(word a,byte d)
 {
  msg.printf(2,"write[%X]=%X",a,d);
  switch (a)
  {
   case 0x8000:
   // rp.page[!(d&0x10) ? 0 : 1]=d&0x7;
//    rp.page[0]=(d&0xF);
//    rp.page[1]=(d&0xF);
//    setrompages();
    rp.page[0]=d>>4;
    rp.page[1]=(d&0xF);
    setrompages();

    return 1;
   case 0xC000:
//    rp.page[2]=(d&0xF);
    //rp.page[!(d&0x20) ? 2 : 3]=d&0x7;
//    rp.page[0]=d>>4;
//    rp.page[1]=16-(d&0xF);
//    rp.page[3]=d>>4;
   return 1;
  }

  return 0;
 }

 virtual int write5000(word a,byte d)
 {
  msg.printf(2,"write[%X]=%X",a,d);
  return 0;
 }

};





//------------------------------------------
//create memory mapper
memorymapper *newmemorymapper(int type)
{
 switch (type)
 {
  case MMC_KONAMI: return new mmc_konami;
  case MMC_SEQ: return new mmc_sequential;

  case MMC_VROMSWITCH: return new mmc_vromswitch;
  case MMC_NONE: return new mmc_none;
  case MMC_5202: return new mmc_5202;
  case MMC_CASTLE3: return new mmc_castlevania3;
  case MMC_F4xxx: return new mmc_f4xxx;

  default:
   msg.error("#%d: %s Mapper not supported",type,getmmctypestr(type));
   return new mmc_none;
 };
}


//-------------------------------

char *banktypestr[]=
{
 "None",    //0
 "MMC1",  //1
 "MMC2",   //2
 "VROM Switch", //3
 "MMC3B", //4
 "MMC5", //5
 "F400x", //6
 "F5xxx", //7

 0,0,0,0,0,0,0,0,0,0,0,0,0
};

char *getmmctypestr(int type)
{
 return banktypestr[type] ? banktypestr[type] : "Unknown";
}





