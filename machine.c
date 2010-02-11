/*
**  Oricutron
**  Copyright (C) 2009 Peter Gordon
**
**  This program is free software; you can redistribute it and/or
**  modify it under the terms of the GNU General Public License
**  as published by the Free Software Foundation, version 2
**  of the License.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with this program; if not, write to the Free Software
**  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**
**  Oric machine stuff
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "system.h"
#include "6502.h"
#include "via.h"
#include "8912.h"
#include "gui.h"
#include "disk.h"
#include "machine.h"
#include "monitor.h"
#include "avi.h"

extern SDL_Surface *screen;
extern int pixpitch;
SDL_bool needclr = SDL_TRUE;
extern Uint16 gpal[];
extern SDL_bool showfps, warpspeed, soundavailable, soundon;

struct avi_handle *vidcap = NULL;
char vidcapname[128];
int vidcapcount = 0;

extern struct osdmenuitem hwopitems[];

unsigned char rom_microdisc[8912], rom_jasmin[2048];
SDL_bool microdiscrom_valid, jasminrom_valid;

Uint8 oricpalette[] = { 0x00, 0x00, 0x00,
                        0xff, 0x00, 0x00,
                        0x00, 0xff, 0x00,
                        0xff, 0xff, 0x00,
                        0x00, 0x00, 0xff,
                        0xff, 0x00, 0xff,
                        0x00, 0xff, 0xff,
                        0xff, 0xff, 0xff };

// Switch between emulation/monitor/menus etc.
void setemumode( struct machine *oric, struct osdmenuitem *mitem, int mode )
{
  oric->emu_mode = mode;

  switch( mode )
  {
    case EM_RUNNING:
      SDL_EnableKeyRepeat( 0, 0 );
      SDL_EnableUNICODE( SDL_FALSE );
      oric->ay.soundon = soundavailable && soundon && (!warpspeed);
      if( oric->ay.soundon )
        SDL_PauseAudio( 0 );
      break;

    case EM_MENU:
      gotomenu( oric, NULL, 0 );

    case EM_DEBUG:
      mon_enter( oric );
      SDL_EnableKeyRepeat( SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL );
      SDL_EnableUNICODE( SDL_TRUE );
      oric->ay.soundon = SDL_FALSE;
      if( soundavailable )
        SDL_PauseAudio( 1 );
      break;
  }
}

// Refresh the video base pointer
static inline void video_refresh_charset( struct machine *oric )
{
  if( oric->vid_textattrs & 1 )
  {
    oric->vid_ch_data = &oric->vid_ch_base[128*8];
  } else {
    oric->vid_ch_data = oric->vid_ch_base;
  }
}

// Decode an oric video attribute
void video_decode_attr( struct machine *oric, int attr )
{
  switch( attr & 0x18 )
  {
    case 0x00: // Foreground colour
      oric->vid_fg_col = attr & 0x07;
      break;

    case 0x08: // Text attribute
      oric->vid_textattrs = attr & 0x07;
      oric->vid_blinkmask = attr & 0x04 ? 0x00 : 0x3f;
      video_refresh_charset( oric );
      break;

    case 0x10: // Background colour
      oric->vid_bg_col = attr & 0x07;
      break;

    case 0x18: // Video mode
      oric->vid_mode = attr & 0x07;
      
      // Set up pointers for new mode
      if( oric->vid_mode & 4 )
      {
        oric->vid_addr = oric->vidbases[0];
        oric->vid_ch_base = &oric->mem[oric->vidbases[1]];
      } else {
        oric->vid_addr = oric->vidbases[2];
        oric->vid_ch_base = &oric->mem[oric->vidbases[3]];
      }

      video_refresh_charset( oric );
      break;
  }   
}

// Render a 6x1 block
void video_render_block( struct machine *oric, int fg, int bg, SDL_bool inverted, int data )
{
  int i;

  if( inverted )
  {
    fg ^= 0x07;
    bg ^= 0x07;
  }
  
  for( i=0; i<6; i++ )
  {
    *(oric->scrpt++) = /*oric->pal[*/ (data & 0x20) ? fg : bg /*]*/;
    data <<= 1;
  }
}

// Copy the video output buffer to the SDL surface
void video_show( struct machine *oric )
{
  int x, y;
  Uint8 *sptr;
  Uint16 *dptr;
  Uint32 *dptr2, *dptr3, c, pp2;

  if( !oric->scr )
    return;
    
  pp2 = pixpitch/2;

  if( ( oric->vid_double ) && ( oric->emu_mode != EM_DEBUG ) )
  {
    if( needclr )
    {
      dptr = (Uint16 *)screen->pixels;
      for( y=0; y<480; y++ )
      {
        for( x=0; x<640; x++ )
          *(dptr++) = gpal[4];
        dptr += pixpitch-640;
      }
      needclr = SDL_FALSE;
    }
    sptr = oric->scr;
    dptr2 = (Uint32 *)(&((Uint16 *)screen->pixels)[320-240+pixpitch*(240-224)]);
    dptr3 = dptr2+pp2;
    for( y=0; y<224; y++ )
    {
      for( x=0; x<240; x++ )
      {
        c = oric->dpal[*(sptr++)];
        *(dptr2++) = c;
        *(dptr3++) = c;
      }
      dptr2 += pixpitch-240;
      dptr3 += pixpitch-240;
    }
    return;
  }

  needclr = SDL_TRUE;

  sptr = oric->scr;
  dptr = (Uint16 *)screen->pixels;

  for( y=0; y<4; y++ )
  {
    memset( dptr, 0, 480 );
    dptr += pixpitch;
  }
  for( ; y<228; y++ )
  {
    for( x=0; x<240; x++ )
      *(dptr++) = oric->pal[*(sptr++)];
    dptr += pixpitch-240;
  }
}

// Draw one rasterline
SDL_bool video_doraster( struct machine *oric )
{
  int b, c, bitmask;
  SDL_bool hires, needrender;
  unsigned int y, cy;
  Uint8 *rptr;

  needrender = SDL_FALSE;

  oric->vid_raster++;
  if( oric->vid_raster == oric->vid_maxrast )
  {
    if( vidcap )
    {
      SDL_LockAudio();
      avi_addframe( &vidcap, oric->scr );
      SDL_UnlockAudio();
    }

    oric->vid_raster = 0;
    needrender = SDL_TRUE;
    oric->frames++;

    if( oric->vid_freq != (oric->vid_mode&2) )
    {
      oric->vid_freq = oric->vid_mode&2;

      // PAL50 = 50Hz = 1,000,000/50 = 20000 cpu cycles/frame
      // 312 scanlines/frame, so 20000/312 = ~64 cpu cycles / scanline

      // PAL60 = 60Hz = 1,000,000/60 = 16667 cpu cycles/frame
      // 312 scanlines/frame, so 16667/312 = ~53 cpu cycles / scanline

      // NTSC = 60Hz = 1,000,000/60 = 16667 cpu cycles/frame
      // 262 scanlines/frame, so 16667/262 = ~64 cpu cycles / scanline
      if( oric->vid_freq )
      {
        // PAL50
        oric->cyclesperraster = 64;
        oric->vid_start       = 65;
        oric->vid_maxrast     = 312;
        oric->vid_special     = oric->vid_start + 200;
        oric->vid_end         = oric->vid_start + 224;
      } else {
        // PAL60
        oric->cyclesperraster = 64;
        oric->vid_start       = 32;
        oric->vid_maxrast     = 262;
        oric->vid_special     = oric->vid_start + 200;
        oric->vid_end         = oric->vid_start + 224;
      }
    }
  }

  // Are we on a visible rasterline?
  if( ( oric->vid_raster < oric->vid_start ) ||
      ( oric->vid_raster >= oric->vid_end ) ) return needrender;

  y = oric->vid_raster - oric->vid_start;

  oric->scrpt = &oric->scr[y*240];
  
  cy = ((oric->vid_raster - oric->vid_start)>>3) * 40;

  // Always start each scanline with white on black
  video_decode_attr( oric, 0x07 );
  video_decode_attr( oric, 0x08 );
  video_decode_attr( oric, 0x10 );

//  if( oric->vid_raster == oric->vid_special )
//    oric->vid_addr = 0xbf68;
  
  if( oric->vid_raster < oric->vid_special )
  {
    if( oric->vid_mode & 0x04 ) // HIRES?
    {
      hires = SDL_TRUE;
      rptr = &oric->mem[oric->vid_addr + y*40 -1];
    } else {
      hires = SDL_FALSE;
      rptr = &oric->mem[oric->vid_addr + cy -1];
    }
  } else {
    hires = SDL_FALSE;

//      read_addr = oric->vid_addr + b + ((y-200)>>3);
    rptr = &oric->mem[oric->vidbases[2] + cy -1];  // bb80 = bf68 - (200/8*40)
  }
  bitmask = (oric->frames&0x10)?0x3f:oric->vid_blinkmask;
    
  for( b=0; b<40; b++ )
  {
    c = *(++rptr);

    /* if bits 6 and 5 are zero, the byte contains a serial attribute */
    if( ( c & 0x60 ) == 0 )
    {
      video_decode_attr( oric, c );
      video_render_block( oric, oric->vid_fg_col, oric->vid_bg_col, (c & 0x80)!=0, 0 );
      if( oric->vid_raster < oric->vid_special )
      {
        if( oric->vid_mode & 0x04 ) // HIRES?
        {
          hires = SDL_TRUE;
          rptr = &oric->mem[oric->vid_addr + b + y*40];
        } else {
          hires = SDL_FALSE;
          rptr = &oric->mem[oric->vid_addr + b + cy];
        }
      } else {
        hires = SDL_FALSE;

//        read_addr = oric->vid_addr + b + ((y-200)>>3);
        rptr = &oric->mem[oric->vidbases[2] + b + cy];   // bb80 = bf68 - (200/8*40)
      }
      bitmask = (oric->frames&0x10)?0x3f:oric->vid_blinkmask;
    
    } else {
      if( hires )
      {
        video_render_block( oric, oric->vid_fg_col, oric->vid_bg_col, (c & 0x80)!=0, c & bitmask );
      } else {
        int ch_ix, ch_dat, ch_line;
          
        ch_ix   = c & 0x7f;
        if( oric->vid_textattrs & 0x02 )
          ch_line = (y>>1) & 0x07;
        else
          ch_line = y & 0x07;
          
        ch_dat = oric->vid_ch_data[ (ch_ix<<3) | ch_line ] & bitmask;
        
        video_render_block( oric, oric->vid_fg_col, oric->vid_bg_col, (c & 0x80)!=0, ch_dat  );
      }
    }
  }

  return needrender;
}

// Oric Atmos CPU write
void atmoswrite( struct m6502 *cpu, unsigned short addr, unsigned char data )
{
  struct machine *oric = (struct machine *)cpu->userdata;
  if( ( !oric->romdis ) && ( addr >= 0xc000 ) ) return;  // Can't write to ROM!
  if( ( addr & 0xff00 ) == 0x0300 )
  {
    via_write( &oric->via, addr, data );
    return;
  }
  oric->mem[addr] = data;
}

// 16k oric-1 CPU write
void o16kwrite( struct m6502 *cpu, unsigned short addr, unsigned char data )
{
  struct machine *oric = (struct machine *)cpu->userdata;
  if( ( !oric->romdis ) && ( addr >= 0xc000 ) ) return;  // Can't write to ROM!
  if( ( addr & 0xff00 ) == 0x0300 )
  {
    via_write( &oric->via, addr, data );
    return;
  }

  oric->mem[addr&0x3fff] = data;
}

// Atmos + jasmin
void jasmin_atmoswrite( struct m6502 *cpu, unsigned short addr, unsigned char data )
{
  struct machine *oric = (struct machine *)cpu->userdata;

  if( oric->jasmin.olay == 0 )
  {
    if( oric->romdis )
    {
      if( addr >= 0xf800 ) return; // Can't write to jasmin rom
    } else {
      if( addr >= 0xc000 ) return; // Can't write to BASIC rom
    }
  }
  
  if( ( addr & 0xff00 ) == 0x0300 )
  {
    if( ( addr >= 0x3f4 ) && ( addr < 0x400 ) )
    {
      jasmin_write( &oric->jasmin, addr, data );
      return;
    }

    via_write( &oric->via, addr, data );
    return;
  }
  oric->mem[addr] = data;
}

// 16k + jasmin
void jasmin_o16kwrite( struct m6502 *cpu, unsigned short addr, unsigned char data )
{
  struct machine *oric = (struct machine *)cpu->userdata;

  if( oric->jasmin.olay == 0 )
  {
    if( oric->romdis )
    {
      if( addr >= 0xf800 ) return; // Can't write to jasmin rom
    } else {
      if( addr >= 0xc000 ) return; // Can't write to BASIC rom
    }
  }

  if( ( addr & 0xff00 ) == 0x0300 )
  {
    if( ( addr >= 0x3f4 ) && ( addr < 0x400 ) )
    {
      jasmin_write( &oric->jasmin, addr, data );
      return;
    }

    via_write( &oric->via, addr, data );
    return;
  }

  oric->mem[addr&0x3fff] = data;
}

// Atmos + microdisc
void microdisc_atmoswrite( struct m6502 *cpu, unsigned short addr, unsigned char data )
{
  struct machine *oric = (struct machine *)cpu->userdata;
  if( oric->romdis )
  {
    if( ( oric->md.diskrom ) && ( addr >= 0xe000 ) ) return; // Can't write to ROM!
  } else {
    if( addr >= 0xc000 ) return;
  }

  if( ( addr & 0xff00 ) == 0x0300 )
  {
    if( ( addr >= 0x310 ) && ( addr < 0x31c ) )
    {
      microdisc_write( &oric->md, addr, data );
    } else {
      via_write( &oric->via, addr, data );
    }
    return;
  }
  oric->mem[addr] = data;
}

// Atmos + microdisc
void microdisc_o16kwrite( struct m6502 *cpu, unsigned short addr, unsigned char data )
{
  struct machine *oric = (struct machine *)cpu->userdata;
  if( oric->romdis )
  {
    if( ( oric->md.diskrom ) && ( addr >= 0xe000 ) ) return; // Can't write to ROM!
  } else {
    if( addr >= 0xc000 ) return;
  }

  if( ( addr & 0xff00 ) == 0x0300 )
  {
    if( ( addr >= 0x310 ) && ( addr < 0x31c ) )
    {
      microdisc_write( &oric->md, addr, data );
    } else {
      via_write( &oric->via, addr, data );
    }
    return;
  }
  oric->mem[addr&0x3fff] = data;
}

// VIA is returned as RAM since it isn't ROM
SDL_bool isram( struct machine *oric, unsigned short addr )
{
  if( addr < 0xc000 ) return SDL_TRUE;
  
  if( !oric->romdis ) return SDL_FALSE;
  
  switch( oric->drivetype )
  {
    case DRV_MICRODISC:
      if( !oric->md.diskrom ) return SDL_TRUE;
      if( addr >= 0xe000 ) return SDL_FALSE;
      break;
    
    case DRV_JASMIN:
      if( oric->jasmin.olay ) return SDL_TRUE;
      if( addr >= 0xf800 ) return SDL_FALSE;
      break;
  }
  
  return SDL_TRUE;
}

// Oric Atmos CPU read
unsigned char atmosread( struct m6502 *cpu, unsigned short addr )
{
  struct machine *oric = (struct machine *)cpu->userdata;

  if( ( addr & 0xff00 ) == 0x0300 )
    return via_read( &oric->via, addr );

  if( ( !oric->romdis ) && ( addr >= 0xc000 ) )
    return oric->rom[addr-0xc000];

  return oric->mem[addr];
}

// Oric-1 16K CPU read
unsigned char o16kread( struct m6502 *cpu, unsigned short addr )
{
  struct machine *oric = (struct machine *)cpu->userdata;

  if( ( addr & 0xff00 ) == 0x0300 )
    return via_read( &oric->via, addr );

  if( ( !oric->romdis ) && ( addr >= 0xc000 ) )
    return oric->rom[addr-0xc000];

  return oric->mem[addr&0x3fff];
}

// Atmos + jasmin
unsigned char jasmin_atmosread( struct m6502 *cpu, unsigned short addr )
{
  struct machine *oric = (struct machine *)cpu->userdata;

  if( oric->jasmin.olay == 0 )
  {
    if( oric->romdis )
    {
      if( addr >= 0xf800 ) return rom_jasmin[addr-0xf800];
    } else {
      if( addr >= 0xc000 ) return oric->rom[addr-0xc000];
    }
  }
  
  if( ( addr & 0xff00 ) == 0x0300 )
  {
    if( ( addr >= 0x3f4 ) && ( addr < 0x400 ) )
      return jasmin_read( &oric->jasmin, addr );

    return via_read( &oric->via, addr );
  }

  return oric->mem[addr];
}

// 16k + jasmin
unsigned char jasmin_o16kread( struct m6502 *cpu, unsigned short addr )
{
  struct machine *oric = (struct machine *)cpu->userdata;

  if( oric->jasmin.olay == 0 )
  {
    if( oric->romdis )
    {
      if( addr >= 0xf800 ) return rom_jasmin[addr-0xf800];
    } else {
      if( addr >= 0xc000 ) return oric->rom[addr-0xc000];
    }
  }

  if( ( addr & 0xff00 ) == 0x0300 )
  {
    if( ( addr >= 0x3f4 ) && ( addr < 0x400 ) )
      return jasmin_read( &oric->jasmin, addr );

    return via_read( &oric->via, addr );
  }

  return oric->mem[addr&0x3fff];
}

// Atmos + microdisc
unsigned char microdisc_atmosread( struct m6502 *cpu, unsigned short addr )
{
  struct machine *oric = (struct machine *)cpu->userdata;

  if( oric->romdis )
  {
    if( ( oric->md.diskrom ) && ( addr >= 0xe000 ) )
      return rom_microdisc[addr-0xe000];
  } else {
    if( addr >= 0xc000 )
      return oric->rom[addr-0xc000];
  }

  if( ( addr & 0xff00 ) == 0x0300 )
  {
    if( ( addr >= 0x310 ) && ( addr < 0x31c ) )
      return microdisc_read( &oric->md, addr );

    return via_read( &oric->via, addr );
  }

  return oric->mem[addr];
}

// Atmos + microdisc
unsigned char microdisc_o16kread( struct m6502 *cpu, unsigned short addr )
{
  struct machine *oric = (struct machine *)cpu->userdata;

  if( oric->romdis )
  {
    if( ( oric->md.diskrom ) && ( addr >= 0xe000 ) )
      return rom_microdisc[addr-0xe000];
  } else {
    if( addr >= 0xc000 )
      return oric->rom[addr-0xc000];
  }

  if( ( addr & 0xff00 ) == 0x0300 )
  {
    if( ( addr >= 0x310 ) && ( addr < 0x31c ) )
      return microdisc_read( &oric->md, addr );

    return via_read( &oric->via, addr );
  }

  return oric->mem[addr&0x3fff];
}

static SDL_bool load_rom( char *fname, int size, unsigned char *where )
{
  FILE *f;

  f = fopen( fname, "rb" );
  if( !f )
  {
    printf( "Unable to open '%s'\n", fname );
    return SDL_FALSE;
  }

  if( fread( where, size, 1, f ) != 1 )
  {
    fclose( f );
    printf( "Unable to read '%s'\n", fname );
    return SDL_FALSE;
  }

  fclose( f );
  return SDL_TRUE;
}

void preinit_machine( struct machine *oric )
{
  int i;

  oric->mem = NULL;
  oric->rom = NULL;
  oric->scr = NULL;

  oric->tapebuf = NULL;
  oric->tapelen = 0;
  oric->tapemotor = SDL_FALSE;
  oric->tapeturbo = SDL_TRUE;
  oric->autorewind = SDL_FALSE;
  oric->autoinsert = SDL_TRUE;
  oric->symbolsautoload = SDL_TRUE;
  oric->symbolscase = SDL_FALSE;
  oric->tapename[0] = 0;

  oric->drivetype = DRV_NONE;
  for( i=0; i<MAX_DRIVES; i++ )
  {
    oric->wddisk.disk[i] = NULL;
    oric->diskname[i][0] = 0;
  }

  microdiscrom_valid = load_rom( ROMPREFIX"microdis.rom", 8192, rom_microdisc );
  jasminrom_valid    = load_rom( ROMPREFIX"jasmin.rom"  , 2048, rom_jasmin );
}

SDL_bool emu_event( SDL_Event *ev, struct machine *oric, SDL_bool *needrender )
{
//  char stmp[32];
  switch( ev->type )
  {
/*
    case SDL_USEREVENT:
      *needrender = SDL_TRUE;
      break;
*/
    case SDL_MOUSEBUTTONDOWN:
      if( ( ev->button.button == SDL_BUTTON_LEFT ) ||
          ( ev->button.button == SDL_BUTTON_RIGHT ) )
        setemumode( oric, NULL, EM_MENU );
      *needrender = SDL_TRUE;
      break;

    case SDL_KEYUP:
      switch( ev->key.keysym.sym )
      {
        case SDLK_F1:
          setemumode( oric, NULL, EM_MENU );
          *needrender = SDL_TRUE;
          break;
 
        case SDLK_F2:
          setemumode( oric, NULL, EM_DEBUG );
          *needrender = SDL_TRUE;
          break;
        
        case SDLK_F4:
          if( oric->drivetype != DRV_JASMIN ) break;
          
          oric->cpu.write( &oric->cpu, 0x3fb, 1 ); // ROMDIS
          m6502_reset( &oric->cpu );
          break;
        
        case SDLK_F5:
          showfps = showfps ? SDL_FALSE : SDL_TRUE;
          if( !showfps ) needclr = SDL_TRUE;
          break;

        case SDLK_F6:
          warpspeed = warpspeed ? SDL_FALSE : SDL_TRUE;
          if( soundavailable && soundon )
          {
            oric->ay.soundon = !warpspeed;
            SDL_PauseAudio( warpspeed );
          }
          break;
        
        case SDLK_F10:
           if( vidcap )
           {
             avi_close( &vidcap );
             do_popup( "AVI capture stopped" );
             break;
           }

           sprintf( vidcapname, "Capturing to video%02d.avi", vidcapcount );
           SDL_LockAudio();
           vidcap = avi_open( &vidcapname[13], oricpalette );
           SDL_UnlockAudio();
           if( vidcap )
           {
             vidcapcount++;
             do_popup( vidcapname );
           }
           break;

        default:
          ay_keypress( &oric->ay, ev->key.keysym.sym, SDL_FALSE );
          break;
      }
      break;

    case SDL_KEYDOWN:
      ay_keypress( &oric->ay, ev->key.keysym.sym, SDL_TRUE );
      break;
  }

  return SDL_FALSE;
}

void blank_ram( Sint32 how, Uint8 *mem, Uint32 size )
{
  Uint32 i, j;

  switch( how )
  {
    case 0:
      for( i=0; i<size; i+=256 )
      {
        for( j=0; j<128; j++ )
        {
          mem[i+j    ] = 0;
          mem[i+j+128] = 255;
        }
      }
      break;
    
    default:
      for( i=0; i<size; i+=2 )
      {
        mem[i  ] = 0xff;
        mem[i+1] = 0x00;
      }
      break;
  }
}

SDL_bool init_machine( struct machine *oric, int type, SDL_bool nukebreakpoints )
{
  int i;

  oric->type = type;
  m6502_init( &oric->cpu, (void*)oric, nukebreakpoints );

  oric->vidbases[0] = 0xa000;
  oric->vidbases[1] = 0x9800;
  oric->vidbases[2] = 0xbb80;
  oric->vidbases[3] = 0xb400;

  switch( type )
  {
    case MACH_ORIC1_16K:
      for( i=0; i<4; i++ )
        oric->vidbases[i] &= 0x7fff;
      hwopitems[0].name = " Oric-1";
      hwopitems[1].name = "\x0e""Oric-1 16K";
      hwopitems[2].name = " Atmos";
      hwopitems[3].name = " Telestrat";
      oric->mem = malloc( 16384 + 16384 );
      if( !oric->mem )
      {
        printf( "Out of memory\n" );
        return SDL_FALSE;
      }

      blank_ram( 0, oric->mem, 16384+16384 );      

      oric->rom = &oric->mem[16384];

      switch( oric->drivetype )
      {
        case DRV_MICRODISC:
          oric->cpu.read = microdisc_o16kread;
          oric->cpu.write = microdisc_o16kwrite;
          oric->romdis = SDL_TRUE;
          microdisc_init( &oric->md, &oric->wddisk, oric );
          break;
        
        case DRV_JASMIN:
          oric->cpu.read = jasmin_o16kread;
          oric->cpu.write = jasmin_o16kwrite;
          oric->romdis = SDL_FALSE;
          jasmin_init( &oric->jasmin, &oric->wddisk, oric );
          break;

        default:
          oric->cpu.read = o16kread;
          oric->cpu.write = o16kwrite;
          oric->romdis = SDL_FALSE;
          break;
      }

      if( !load_rom( ROMPREFIX"basic10.rom", 16384, &oric->rom[0] ) )
        return SDL_FALSE;

      for( i=0; i<8; i++ )
      {
        oric->pal[i] = SDL_MapRGB( screen->format, oricpalette[i*3], oricpalette[i*3+1], oricpalette[i*3+2] );
        oric->dpal[i] = (oric->pal[i]<<16)|oric->pal[i];
      }

      oric->scr = (Uint8 *)malloc( 240*224 );
      if( !oric->scr ) return SDL_FALSE;

      oric->cyclesperraster = 64;
      oric->vid_start = 65;
      oric->vid_maxrast = 312;
      oric->vid_special = oric->vid_start + 200;
      oric->vid_end     = oric->vid_start + 224;
      oric->vid_raster  = 0;
      video_decode_attr( oric, 0x1a );
      break;
    
    case MACH_ORIC1:
      hwopitems[0].name = "\x0e""Oric-1";
      hwopitems[1].name = " Oric-1 16K";
      hwopitems[2].name = " Atmos";
      hwopitems[3].name = " Telestrat";
      oric->mem = malloc( 65536 + 16384 );
      if( !oric->mem )
      {
        printf( "Out of memory\n" );
        return SDL_FALSE;
      }

      blank_ram( 0, oric->mem, 65536+16384 );      

      oric->rom = &oric->mem[65536];

      switch( oric->drivetype )
      {
        case DRV_MICRODISC:
          oric->cpu.read = microdisc_atmosread;
          oric->cpu.write = microdisc_atmoswrite;
          oric->romdis = SDL_TRUE;
          microdisc_init( &oric->md, &oric->wddisk, oric );
          break;
        
        case DRV_JASMIN:
          oric->cpu.read = jasmin_atmosread;
          oric->cpu.write = jasmin_atmoswrite;
          oric->romdis = SDL_FALSE;
          jasmin_init( &oric->jasmin, &oric->wddisk, oric );
          break;

        default:
          oric->cpu.read = atmosread;
          oric->cpu.write = atmoswrite;
          oric->romdis = SDL_FALSE;
          break;
      }

      if( !load_rom( ROMPREFIX"basic10.rom", 16384, &oric->rom[0] ) )
        return SDL_FALSE;

      oric->pal[0] = SDL_MapRGB( screen->format, 0x00, 0x00, 0x00 );
      oric->pal[1] = SDL_MapRGB( screen->format, 0xff, 0x00, 0x00 );
      oric->pal[2] = SDL_MapRGB( screen->format, 0x00, 0xff, 0x00 );
      oric->pal[3] = SDL_MapRGB( screen->format, 0xff, 0xff, 0x00 );
      oric->pal[4] = SDL_MapRGB( screen->format, 0x00, 0x00, 0xff );
      oric->pal[5] = SDL_MapRGB( screen->format, 0xff, 0x00, 0xff );
      oric->pal[6] = SDL_MapRGB( screen->format, 0x00, 0xff, 0xff );
      oric->pal[7] = SDL_MapRGB( screen->format, 0xff, 0xff, 0xff );

      for( i=0; i<8; i++ )
        oric->dpal[i] = (oric->pal[i]<<16)|oric->pal[i];

      oric->scr = (Uint8 *)malloc( 240*224 );
      if( !oric->scr ) return SDL_FALSE;

      oric->cyclesperraster = 64;
      oric->vid_start = 65;
      oric->vid_maxrast = 312;
      oric->vid_special = oric->vid_start + 200;
      oric->vid_end     = oric->vid_start + 224;
      oric->vid_raster  = 0;
      video_decode_attr( oric, 0x1a );
      break;
    
    case MACH_ATMOS:
      hwopitems[0].name = " Oric-1";
      hwopitems[1].name = " Oric-1 16K";
      hwopitems[2].name = "\x0e""Atmos";
      hwopitems[3].name = " Telestrat";
      oric->mem = malloc( 65536 + 16384 );
      if( !oric->mem )
      {
        printf( "Out of memory\n" );
        return SDL_FALSE;
      }

      blank_ram( 0, oric->mem, 65536+16384 );      

      oric->rom = &oric->mem[65536];

      switch( oric->drivetype )
      {
        case DRV_MICRODISC:
          oric->cpu.read = microdisc_atmosread;
          oric->cpu.write = microdisc_atmoswrite;
          oric->romdis = SDL_TRUE;
          microdisc_init( &oric->md, &oric->wddisk, oric );
          break;
        
        case DRV_JASMIN:
          oric->cpu.read = jasmin_atmosread;
          oric->cpu.write = jasmin_atmoswrite;
          oric->romdis = SDL_FALSE;
          jasmin_init( &oric->jasmin, &oric->wddisk, oric );
          break;

        default:
          oric->cpu.read = atmosread;
          oric->cpu.write = atmoswrite;
          oric->romdis = SDL_FALSE;
          break;
      }

      if( !load_rom( ROMPREFIX"basic11b.rom", 16384, &oric->rom[0] ) )
        return SDL_FALSE;

      oric->pal[0] = SDL_MapRGB( screen->format, 0x00, 0x00, 0x00 );
      oric->pal[1] = SDL_MapRGB( screen->format, 0xff, 0x00, 0x00 );
      oric->pal[2] = SDL_MapRGB( screen->format, 0x00, 0xff, 0x00 );
      oric->pal[3] = SDL_MapRGB( screen->format, 0xff, 0xff, 0x00 );
      oric->pal[4] = SDL_MapRGB( screen->format, 0x00, 0x00, 0xff );
      oric->pal[5] = SDL_MapRGB( screen->format, 0xff, 0x00, 0xff );
      oric->pal[6] = SDL_MapRGB( screen->format, 0x00, 0xff, 0xff );
      oric->pal[7] = SDL_MapRGB( screen->format, 0xff, 0xff, 0xff );

      for( i=0; i<8; i++ )
        oric->dpal[i] = (oric->pal[i]<<16)|oric->pal[i];

      oric->scr = (Uint8 *)malloc( 240*224 );
      if( !oric->scr ) return SDL_FALSE;

      oric->cyclesperraster = 64;
      oric->vid_start = 65;
      oric->vid_maxrast = 312;
      oric->vid_special = oric->vid_start + 200;
      oric->vid_end     = oric->vid_start + 224;
      oric->vid_raster  = 0;
      video_decode_attr( oric, 0x1a );
      break;

    case MACH_TELESTRAT:
      hwopitems[0].name = " Oric-1";
      hwopitems[1].name = " Oric-1 16K";
      hwopitems[2].name = " Atmos";
      hwopitems[3].name = "\x0e""Telestrat";
      printf( "Telestrat not implimented yet\n" );
      return SDL_FALSE;
  }

  oric->tapename[0] = 0;
  tape_rewind( oric );
  m6502_reset( &oric->cpu );
  via_init( &oric->via, oric );
  ay_init( &oric->ay, oric );
  oric->cpu.rastercycles = oric->cyclesperraster;
  oric->frames = 0;
  oric->vid_double = SDL_TRUE;
  setemumode( oric, NULL, EM_RUNNING );

  if( oric->autorewind ) tape_rewind( oric );

  setmenutoggles( oric );

  return SDL_TRUE;
}

void shut_machine( struct machine *oric )
{
  if( oric->drivetype == DRV_MICRODISC ) { microdisc_free( &oric->md ); oric->drivetype = DRV_NONE; }
  if( oric->drivetype == DRV_JASMIN )    { jasmin_free( &oric->jasmin ); oric->drivetype = DRV_NONE; }
  if( oric->mem ) { free( oric->mem ); oric->mem = NULL; oric->rom = NULL; }
  if( oric->scr ) { free( oric->scr ); oric->scr = NULL; }
}

void shut( void );
void setdrivetype( struct machine *oric, struct osdmenuitem *mitem, int type )
{
  if( oric->drivetype == type )
    return;

  shut_machine( oric );

  switch( type )
  {
    case DRV_MICRODISC:
    case DRV_JASMIN:
      oric->drivetype = type;
      break;
    
    default:
      oric->drivetype = DRV_NONE;
      break;
  }

  mon_watch_reset( oric );
  if( !init_machine( oric, oric->type, SDL_FALSE ) )
  {
    shut();
    exit(0);
  }

  setmenutoggles( oric );
}

void swapmach( struct machine *oric, struct osdmenuitem *mitem, int which )
{
  int curr_drivetype;
  
  curr_drivetype = oric->drivetype;

  shut_machine( oric );

  if( ((which>>16)&0xffff) != 0xffff )
    curr_drivetype = (which>>16)&0xffff;

  which &= 0xffff;

  oric->drivetype = curr_drivetype;

  mon_watch_reset( oric );
  if( !init_machine( oric, which, which!=oric->type ) )
  {
    shut();
    exit(0);
  }
}

