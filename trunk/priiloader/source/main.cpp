/*

priiloader/preloader 0.30 - A tool which allows to change the default boot up sequence on the Wii console

Copyright (C) 2008-2009  crediar

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.


*/
//#define DEBUG

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string>


#include <gccore.h>
#include <wiiuse/wpad.h>
#include <sdcard/wiisd_io.h>
#include <fat.h>
#include <ogc/usb.h>
#include <ogc/machine/processor.h>
#include <ogc/machine/asm.h>
#include "usbstorage.h"

#include <sys/dir.h>
#include <malloc.h>
#include <vector>
#include <time.h>

#include <mp3player.h>
#include <asndlib.h>

//Project files
#include "../../Shared/svnrev.h"
#include "Global.h"
#include "settings.h"
#include "state.h"
#include "elf.h"
#include "error.h"
#include "hacks.h"
#include "font.h"
#include "gecko.h"
#include "password.h"
#include "sha1.h"
#include "HTTP_Parser.h"


//Bin includes
#include "certs_bin.h"
#include "stub_bin.h"

extern "C"
{
	extern void _unstub_start(void);
}
typedef struct {
	unsigned int offsetText[7];
	unsigned int offsetData[11];
	unsigned int addressText[7];
	unsigned int addressData[11];
	unsigned int sizeText[7];
	unsigned int sizeData[11];
	unsigned int addressBSS;
	unsigned int sizeBSS;
	unsigned int entrypoint;
} dolhdr;

extern Settings *settings;
extern u8 error;
extern std::vector<hack> hacks;
extern std::vector<hack_hash> hacks_hash;
extern u32 *states;
extern u32 *states_hash;

u8 Shutdown=0;
u8 BootSysMenu = 0;
u8 ReloadedIOS = 0;
s8 Mounted = 0;
s8 Device_Not_Mountable = 0;
time_t startloop;

s32 __IOS_LoadStartupIOS()
{
        return 0;
}
u8 DetectHBC( void )
{
    u64 *list;
    u32 titlecount;
    s32 ret;

    ret = ES_GetNumTitles(&titlecount);
    if(ret < 0)
	{
		gprintf("DetectHBC : ES_GetNumTitles failure\n");
		return 0;
	}

    list = (u64*)memalign(32, titlecount * sizeof(u64) + 32);

    ret = ES_GetTitles(list, titlecount);
    if(ret < 0) {
		gprintf("DetectHBC :ES_GetTitles failure\n");
		free_pointer(list);
		return 0;
    }
	ret = 0;
	//lets check for known HBC title id's.
    for(u32 i=0; i<titlecount; i++) 
	{
		if (list[i] == 0x00010001AF1BF516LL)
		{
			if(ret < 3)
				ret = 3;
		}
		if (list[i] == 0x000100014A4F4449LL)
		{
			if (ret < 2)
				ret = 2;
		}
        if (list[i] == 0x0001000148415858LL)
        {
			if (ret < 1)
				ret = 1;
        }
    }
	free_pointer(list);
    if(ret < 1)
	{
		gprintf("HBC not found\n");
	}
	return ret;
}
void LoadHBCStub ( void )
{
	//LoadHBCStub: Load HBC JODI reload Stub and change stub to haxx if needed. 
	//the first part of the title is at 0x800024CA (first 2 bytes) and 0x800024D2 (last 2 bytes)
	//HBC < 1.0.5 = HAXX or 4841 5858
	//HBC >= 1.0.5 = JODI or 4A4F 4449
	if ( *(vu32*)0x80001804 == 0x53545542 && *(vu32*)0x80001808 == 0x48415858 )
	{
		return;
	}
	//load Stub, contains JODI by default.
	memcpy((void*)0x80001800, stub_bin, stub_bin_size);
	DCFlushRange((void*)0x80001800,stub_bin_size);
	
	//see if changes are needed to change it to HAXX
    switch(DetectHBC())
	{
		case 3:
			gprintf("changing stub to load HBC...\n");
			*(vu16*)0x800024CA = 0xAF1B;
			*(vu16*)0x800024D2 = 0xF516;
			break;
		case 1: //HAXX
			gprintf("changing stub to load HAXX...\n");
			*(vu16*)0x800024CA = 0x4841;//"HA";
			*(vu16*)0x800024D2 = 0x5858;//"XX";
		case 2: //JODI, no changes are needed
		default: //not good, no HBC was detected >_> lets keep the stub anyway
			break;
	}
	gprintf("HBC stub : Loaded\n");
	return;	
}
void UnloadHBCStub( void )
{
	//some apps apparently dislike it if the stub stays in memory but for some reason isn't active :/
	memset((void*)0x80001800, 0, stub_bin_size);
	DCFlushRange((void*)0x80001800,stub_bin_size);	
}
bool PollDevices( void )
{
	//check mounted device's status and unmount or mount if needed. once something is unmounted, lets see if we can mount something in its place
	if( ( (Mounted & 2) && !__io_wiisd.isInserted() ) || ( (Mounted & 1) && !__io_usbstorage.isInserted() ) )
	{
		fatUnmount("fat:/");
		if(Mounted & 1)
		{
			gprintf("USB removed. unmounting...\n");
		}
		if(Mounted & 2)
		{
			gprintf("SD removed. unmounting...\n");
			__io_wiisd.shutdown();
		}			
		Mounted = 0;
	}
	//check if SD is mountable
	if( !(Mounted & 2) && __io_wiisd.startup() &&  __io_wiisd.isInserted() && !(Device_Not_Mountable & 2) )
	{
		if(Mounted & 1)
		{
			//USB is mounted. lets kick it out and use SD instead :P
			fatUnmount("fat:/");
			gprintf("USB unmounted for SD\n");
			Mounted = 0;
		}
		if(fatMountSimple("fat",&__io_wiisd))
		{
			Mounted |= 2;
			gprintf("SD inserted and mounted\n");
		}
		else
		{
			gprintf("inserted SD failed to mount : not fat?\n");
			Device_Not_Mountable |= 2;
		}
	}
	//check if USB is mountable.deu to short circuit evaluation you need to be VERY CAREFUL when changing the next if or anything usbstorage related
	//i know its stupid to init the USB device and yet not mount it, but thats the only way with c++ & the current usbstorage combo
	//see http://en.wikipedia.org/wiki/Short-circuit_evaluation
	else if( ( __io_usbstorage.startup() ) && ( __io_usbstorage.isInserted() ) && (Mounted == 0) && !(Device_Not_Mountable & 1) )
	{
		//if( fatMountSimple("fat", &__io_usbstorage) )
		if( fatMount("fat", &__io_usbstorage,0, 8, 64) )
		{
			gprintf("USB inserted and mounted\n");
			Mounted |= 1;
		}
		else
		{
			gprintf("inserted USB device failed to mount : not fat?\n");
			Device_Not_Mountable |= 1;
		}
	}
	if ( Device_Not_Mountable > 0 )
	{
		if ( ( Device_Not_Mountable & 1 ) && !__io_usbstorage.isInserted() )
		{
			gprintf("reseting not-mountable-USB flag\n");
			Device_Not_Mountable -= 1;
		}
		if ( ( Device_Not_Mountable & 2 ) &&  !__io_wiisd.isInserted() )
		{
			//not needed for SD yet but just to be on the safe side
			gprintf("reseting not-mountable-SD flag\n");
			Device_Not_Mountable -= 2;
			__io_wiisd.shutdown();
		}
	}
	if ( Mounted > 0)
		return true;
	else
		return false;
}
void ShutdownDevices()
{
	//unmount device
	if(Mounted != 0)
	{
		fatUnmount("fat:/");
		Mounted = 0;
	}
	__io_usbstorage.shutdown();
	__io_wiisd.shutdown();	
	return;
}
bool RemountDevices( void )
{
	ShutdownDevices();
	return PollDevices();
}
void ClearScreen()
{
	if( !SGetSetting(SETTING_BLACKBACKGROUND))
		VIDEO_ClearFrameBuffer( rmode, xfb, 0xFF80FF80);
	else
		VIDEO_ClearFrameBuffer( rmode, xfb, COLOR_BLACK);

	VIDEO_WaitVSync();
	printf("\n");
}
bool isIOSstub(u8 ios_number)
{
	u32 tmd_size = NULL;
	tmd_view *ios_tmd;

	ES_GetTMDViewSize(0x0000000100000000ULL | ios_number, &tmd_size);
	if (!tmd_size)
	{
		//getting size failed. invalid or fake tmd for sure!
		gprintf("isIOSstub : ES_GetTMDViewSize fail ios %d\n",ios_number);
		return true;
	}
	ios_tmd = (tmd_view *)memalign( 32, (tmd_size+31)&(~31) );
	if(!ios_tmd)
	{
		gprintf("isIOSstub : TMD align failure\n");
		return true;
	}
	memset(ios_tmd , 0, tmd_size);
	ES_GetTMDView(0x0000000100000000ULL | ios_number, (u8*)ios_tmd , tmd_size);
	gprintf("isIOSstub : IOS %d is rev %d(0x%x) with tmd size of %u and %u contents\n",ios_number,ios_tmd->title_version,ios_tmd->title_version,tmd_size,ios_tmd->num_contents);
	/*Stubs have a few things in common:
	- title version : it is mostly 65280 , or even better : in hex the last 2 digits are 0. 
		example : IOS 60 rev 6400 = 0x1900 = 00 = stub
	- exception for IOS21 which is active, the tmd size is 592 bytes (or 140 with the views)
	- the stub ios' have 1 app of their own (type 0x1) and 2 shared apps (type 0x8001).
	eventho the 00 check seems to work fine , we'll only use other knowledge as well cause some
	people/applications install an ios with a stub rev >_> ...*/
	u8 Version = ios_tmd->title_version;
	//version now contains the last 2 bytes. as said above, if this is 00, its a stub
	if ( Version == 0 )
	{
		if ( ( ios_tmd->num_contents == 3) && (ios_tmd->contents[0].type == 1 && ios_tmd->contents[1].type == 0x8001 && ios_tmd->contents[2].type == 0x8001) )
		{
			gprintf("isIOSstub : %d is stub",ios_number);
			free_pointer(ios_tmd);
			return true;
		}
		else
		{
			gprintf("isIOSstub : %d is active\n",ios_number);
			free_pointer(ios_tmd);
			return false;
		}
	}
	gprintf("isIOSstub : %d is active\n",ios_number);
	free_pointer(ios_tmd);
	return false;
}

void SysHackSettings( void )
{
	if( !LoadHacks(false) )
	{
		if(Mounted == 0)
		{
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Failed to mount FAT device"))*13/2))>>1, 208+16, "Failed to mount FAT device");
		}
		else
		{
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Can't find Hacks.ini on FAT Device"))*13/2))>>1, 208+16, "Can't find Hacks.ini on FAT Device");
		}
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Can't find Hacks.ini on NAND"))*13/2))>>1, 208+16+16, "Can't find Hacks.ini on NAND");
		sleep(5);
		return;
	}

//Count hacks for current sys version
	u32 HackCount=0;
	u32 SysVersion=GetSysMenuVersion();
	for( unsigned int i=0; i<hacks.size(); ++i)
	{
		if( hacks[i].version == SysVersion )
		{
			HackCount++;
		}
	}

	if( HackCount == 0 )
	{
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Couldn't find any hacks for"))*13/2))>>1, 208, "Couldn't find any hacks for");
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("System Menu version:vxxx"))*13/2))>>1, 228, "System Menu version:v%d", SysVersion );
		sleep(5);
		return;
	}

	u32 DispCount=HackCount;

	if( DispCount > 25 )
		DispCount = 25;

	u16 cur_off=0;
	s32 menu_off=0;
	bool redraw=true;
 
	while(1)
	{
		WPAD_ScanPads();
		PAD_ScanPads();

		u32 WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
		u32 PAD_Pressed  = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);

#ifdef DEBUG
		if ( (WPAD_Pressed & WPAD_BUTTON_HOME) || (PAD_Pressed & PAD_BUTTON_START) )
		{
			exit(0);
		}
#endif
		if ( WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B )
		{
			break;
		}

		if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
		{
			if( cur_off == DispCount)
			{
				//first try to open the file on the SD card/USB, if we found it copy it, other wise skip
				s16 fail = 0;
				FILE *in = NULL;
				if (Mounted != 0)
				{
					in = fopen ("fat:/hacks.ini","rb");
				}
				else
				{
					gprintf("no FAT device found\n");
				}
				if ( ( (Mounted & 2) && !__io_wiisd.isInserted() ) || ( (Mounted & 1) && !__io_usbstorage.isInserted() ) )
				{
					PrintFormat( 0, 103, rmode->viHeight-48, "saving failed : SD/USB error");
					continue;
				}
				if( in != NULL )
				{
					//Read in whole file & create it on nand
					fseek( in, 0, SEEK_END );
					u32 size = ftell(in);
					fseek( in, 0, 0);

					char *buf = (char*)memalign( 32, (size+31)&(~31) );
					memset( buf, 0, (size+31)&(~31) );
					fread( buf, sizeof( char ), size, in );

					fclose(in);

					s32 fd = ISFS_Open("/title/00000001/00000002/data/hacks.ini", 1|2 );
					if( fd >= 0 )
					{
						//File already exists, delete and recreate!
						ISFS_Close( fd );
						if(ISFS_Delete("/title/00000001/00000002/data/hacks.ini") <0)
						{
							fail=1;
							free_pointer(buf);
							goto handle_hacks_fail;
						}
					}
					if(ISFS_CreateFile("/title/00000001/00000002/data/hacks.ini", 0, 3, 3, 3)<0)
					{
						fail=2;
						free_pointer(buf);
						goto handle_hacks_fail;
					}
					fd = ISFS_Open("/title/00000001/00000002/data/hacks.ini", 1|2 );
					if( fd < 0 )
					{
						fail=3;
						ISFS_Close( fd );
						free_pointer(buf);
						goto handle_hacks_fail;
					}

					if(ISFS_Write( fd, buf, size )<0)
					{
						fail = 4;
						ISFS_Close( fd );
						free_pointer(buf);
						goto handle_hacks_fail;
					}
					ISFS_Close( fd );
					free_pointer(buf);
				}
handle_hacks_fail:
				if(fail > 0)
				{
					gprintf("hacks.ini save error %d\n",fail);
				}
				
				s32 fd = ISFS_Open("/title/00000001/00000002/data/hacks_s.ini", 1|2 );

				if( fd >= 0 )
				{
					//File already exists, delete and recreate!
					ISFS_Close( fd );
					if(ISFS_Delete("/title/00000001/00000002/data/hacks_s.ini")<0)
					{
						fail = 5;
						goto handle_hacks_s_fail;
					}
				}

				if(ISFS_CreateFile("/title/00000001/00000002/data/hacks_s.ini", 0, 3, 3, 3)<0)
				{
					fail = 6;
					goto handle_hacks_s_fail;
				}
				fd = ISFS_Open("/title/00000001/00000002/data/hacks_s.ini", 1|2 );
				if( fd < 0 )
				{
					fail=7;
					goto handle_hacks_s_fail;
				}
				if(ISFS_Write( fd, states, sizeof( u32 ) * hacks.size() )<0)
				{
					fail = 8;
					ISFS_Close(fd);
					goto handle_hacks_s_fail;
				}

				ISFS_Close( fd );
handle_hacks_s_fail:
				if(fail > 0)
				{
					gprintf("hacks.ini save error %d\n",fail);
				}

				if( fail )
					PrintFormat( 0, 118, rmode->viHeight-48, "saving failed");
				else
					PrintFormat( 0, 118, rmode->viHeight-48, "settings saved");
			} 
			else 
			{

				s32 j = 0;
				u32 i = 0;
				for(i=0; i<hacks.size(); ++i)
				{
					if( hacks[i].version == SysVersion )
					{
						if( cur_off+menu_off == j++)
							break;
					}
				}

				if(states[i])
					states[i]=0;
				else 
					states[i]=1;

				redraw = true;
			}
		}

		if ( WPAD_Pressed & WPAD_BUTTON_DOWN || WPAD_Pressed & WPAD_CLASSIC_BUTTON_DOWN || PAD_Pressed & PAD_BUTTON_DOWN )
		{
			cur_off++;

			if( cur_off > DispCount )
			{
				cur_off--;
				menu_off++;
			}

			if( cur_off+menu_off > (s32)HackCount )
			{
				cur_off = 0;
				menu_off= 0;
			}
			
			redraw=true;
		} else if ( WPAD_Pressed & WPAD_BUTTON_UP || WPAD_Pressed & WPAD_CLASSIC_BUTTON_UP || PAD_Pressed & PAD_BUTTON_UP )
		{
			if( cur_off == 0 )
			{
				if( menu_off > 0 )
				{
					cur_off++;
					menu_off--;

				} else {

					//if( DispCount < 20 )
					//{
						cur_off=DispCount;
						menu_off=(HackCount-DispCount);

					//} else {

					//	cur_off=DispCount;
					//	menu_off=(HackCount-DispCount)-1;

					//}
				}
			}
			else
				cur_off--;
	
			redraw=true;
		}

		if( redraw )
		{
			//printf("\x1b[2;0HHackCount:%d DispCount:%d cur_off:%d menu_off:%d Hacks:%d   \n", HackCount, DispCount, cur_off, menu_off, hacks.size() );
			u32 j=0;
			u32 skip=menu_off;
			for( u32 i=0; i<hacks.size(); ++i)
			{
				if( hacks[i].version == SysVersion )
				{
					if( skip > 0 )
					{
						skip--;
					} else {

						PrintFormat( cur_off==j, 16, 48+j*16, "%s", hacks[i].desc );

						if( states[i] )
							PrintFormat( cur_off==j, 256, 48+j*16, "enabled ", hacks[i].desc);
						else
							PrintFormat( cur_off==j, 256, 48+j*16, "disabled", hacks[i].desc);
						
						j++;
					}
				}
				if( j >= 25 ) 
					break;
			}

			PrintFormat( cur_off==(signed)DispCount, 118, rmode->viHeight-64, "save settings");

			PrintFormat( 0, 103, rmode->viHeight-48, "                            ");

			redraw = false;
		}

		VIDEO_WaitVSync();
	}

	return;
}
void SysHackHashSettings( void )
{
	if( !LoadHacks_Hash(false) )
	{
		if(Mounted == 0)
		{
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Failed to mount FAT device"))*13/2))>>1, 208+16, "Failed to mount FAT device");
		}
		else
		{
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Can't find hacks_hash.ini on FAT Device"))*13/2))>>1, 208+16, "Can't find hacks_hash.ini on FAT Device");
		}
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Can't find hacks_hash.ini on NAND"))*13/2))>>1, 208+16+16, "Can't find hacks_hash.ini on NAND");
		sleep(5);
		return;
	}

//Count hacks for current sys version
	u32 HackCount=0;
	u32 SysVersion=GetSysMenuVersion();
	for( unsigned int i=0; i<hacks_hash.size(); ++i)
	{
		if( hacks_hash[i].max_version >= SysVersion && hacks_hash[i].min_version <= SysVersion)
		{
			HackCount++;
		}
	}

	if( HackCount == 0 )
	{
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Couldn't find any hacks for"))*13/2))>>1, 208, "Couldn't find any hacks for");
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("System Menu version:vxxx"))*13/2))>>1, 228, "System Menu version:v%d", SysVersion );
		sleep(5);
		return;
	}

	u32 DispCount=HackCount;

	if( DispCount > 25 )
		DispCount = 25;

	u16 cur_off=0;
	s32 menu_off=0;
	bool redraw=true;
	while(1)
	{
		WPAD_ScanPads();
		PAD_ScanPads();

		u32 WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
		u32 PAD_Pressed  = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);

#ifdef DEBUG
		if ( (WPAD_Pressed & WPAD_BUTTON_HOME) || (PAD_Pressed & PAD_BUTTON_START) )
		{
			exit(0);
		}
#endif
		if ( WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B )
		{
			break;
		}

		if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
		{
			if( cur_off == DispCount)
			{
				//first try to open the file on the SD card/USB, if we found it copy it, other wise skip
				s16 fail = 0;
				FILE *in = NULL;
				if (Mounted != 0)
				{
					in = fopen ("fat:/hacks_hash.ini","rb");
				}
				else
				{
					gprintf("no FAT device found\n");
				}
				if ( ( (Mounted & 2) && !__io_wiisd.isInserted() ) || ( (Mounted & 1) && !__io_usbstorage.isInserted() ) )
				{
					PrintFormat( 0, 103, rmode->viHeight-48, "saving failed : SD/USB error");
					continue;
				}
				if( in != NULL )
				{
					//Read in whole file & create it on nand
					fseek( in, 0, SEEK_END );
					u32 size = ftell(in);
					fseek( in, 0, 0);

					char *buf = (char*)memalign( 32, (size+31)&(~31) );
					memset( buf, 0, (size+31)&(~31) );
					fread( buf, sizeof( char ), size, in );

					fclose(in);

					s32 fd = ISFS_Open("/title/00000001/00000002/data/hackshas.ini", 1|2 );
					if( fd >= 0 )
					{
						//File already exists, delete and recreate!
						ISFS_Close( fd );
						if(ISFS_Delete("/title/00000001/00000002/data/hackshas.ini") <0)
						{
							fail=1;
							free_pointer(buf);
							goto handle_hacks_fail;
						}
					}
					if(ISFS_CreateFile("/title/00000001/00000002/data/hackshas.ini", 0, 3, 3, 3)<0)
					{
						fail=2;
						free_pointer(buf);
						goto handle_hacks_fail;
					}
					fd = ISFS_Open("/title/00000001/00000002/data/hackshas.ini", 1|2 );
					if( fd < 0 )
					{
						fail=3;
						ISFS_Close( fd );
						free_pointer(buf);
						goto handle_hacks_fail;
					}

					if(ISFS_Write( fd, buf, size )<0)
					{
						fail = 4;
						ISFS_Close( fd );
						free_pointer(buf);
						goto handle_hacks_fail;
					}
					ISFS_Close( fd );
					free_pointer(buf);
				}
handle_hacks_fail:
				if(fail > 0)
				{
					gprintf("hacks_hash.ini save error %d\n",fail);
				}
				
				s32 fd = ISFS_Open("/title/00000001/00000002/data/hacksh_s.ini", 1|2 );

				if( fd >= 0 )
				{
					//File already exists, delete and recreate!
					ISFS_Close( fd );
					if(ISFS_Delete("/title/00000001/00000002/data/hacksh_s.ini")<0)
					{
						fail = 5;
						goto handle_hacks_s_fail;
					}
				}

				if(ISFS_CreateFile("/title/00000001/00000002/data/hacksh_s.ini", 0, 3, 3, 3)<0)
				{
					fail = 6;
					goto handle_hacks_s_fail;
				}
				fd = ISFS_Open("/title/00000001/00000002/data/hacksh_s.ini", 1|2 );
				if( fd < 0 )
				{
					fail=7;
					goto handle_hacks_s_fail;
				}
				if(ISFS_Write( fd, states_hash, sizeof( u32 ) * hacks_hash.size() )<0)
				{
					fail = 8;
					ISFS_Close(fd);
					goto handle_hacks_s_fail;
				}

				ISFS_Close( fd );
handle_hacks_s_fail:
				if(fail > 0)
				{
					gprintf("hacksh_s.ini save error %d\n",fail);
				}

				if( fail )
					PrintFormat( 0, 118, rmode->viHeight-48, "saving failed");
				else
					PrintFormat( 0, 118, rmode->viHeight-48, "settings saved");
			} 
			else 
			{

				s32 j = 0;
				u32 i = 0;
				for(i=0; i<hacks_hash.size(); ++i)
				{
					if( hacks_hash[i].max_version >= SysVersion && hacks_hash[i].min_version <= SysVersion)
					{
						if( cur_off+menu_off == j++)
							break;
					}
				}

				if(states_hash[i])
					states_hash[i]=0;
				else 
					states_hash[i]=1;

				redraw = true;
			}
		}

		if ( WPAD_Pressed & WPAD_BUTTON_DOWN || WPAD_Pressed & WPAD_CLASSIC_BUTTON_DOWN || PAD_Pressed & PAD_BUTTON_DOWN )
		{
			cur_off++;

			if( cur_off > DispCount )
			{
				cur_off--;
				menu_off++;
			}

			if( cur_off+menu_off > (s32)HackCount )
			{
				cur_off = 0;
				menu_off= 0;
			}
			
			redraw=true;
		} else if ( WPAD_Pressed & WPAD_BUTTON_UP || WPAD_Pressed & WPAD_CLASSIC_BUTTON_UP || PAD_Pressed & PAD_BUTTON_UP )
		{
			if( cur_off == 0 )
			{
				if( menu_off > 0 )
				{
					cur_off++;
					menu_off--;

				} else {

					//if( DispCount < 20 )
					//{
						cur_off=DispCount;
						menu_off=(HackCount-DispCount);

					//} else {

					//	cur_off=DispCount;
					//	menu_off=(HackCount-DispCount)-1;

					//}
				}
			}
			else
				cur_off--;
	
			redraw=true;
		}
		if( redraw )
		{
			//printf("\x1b[2;0HHackCount:%d DispCount:%d cur_off:%d menu_off:%d Hacks:%d   \n", HackCount, DispCount, cur_off, menu_off, hacks_hash.size() );
			u32 j=0;
			u32 skip=menu_off;
			for( u32 i=0; i<hacks_hash.size(); ++i)
			{
				if( hacks_hash[i].max_version >= SysVersion && hacks_hash[i].min_version <= SysVersion)
				{
					if( skip > 0 )
					{
						skip--;
					} else {
						
						PrintFormat( cur_off==j, 16, 48+j*16, "%s", hacks_hash[i].desc.c_str() );

						if( states_hash[i] )
						{
							PrintFormat( cur_off==j, 256, 48+j*16, "enabled ");
						}
						else
						{
							PrintFormat( cur_off==j, 256, 48+j*16, "disabled");
						}
						j++;
					}
				}
				if( j >= 25 ) 
					break;
			}
			PrintFormat( cur_off==(signed)DispCount, 118, rmode->viHeight-64, "save settings");
			PrintFormat( 0, 103, rmode->viHeight-48, "                            ");

			redraw = false;
		}

		VIDEO_WaitVSync();
	}

	return;
}

void shellsort(u64 *a,int n)
{
	int j,i,m;
	u64 mid;
	for(m = n/2;m>0;m/=2)
	{
		for(j = m;j< n;j++)
		{
			for(i=j-m;i>=0;i-=m)
			{
				if(a[i+m]>=a[i])
					break;
				else
				{
					mid = a[i];
					a[i] = a[i+m];
					a[i+m] = mid;
				}
			}
		}
	}
}
void SetSettings( void )
{
	//clear screen and reset the background
	ClearScreen();

	//get a list of all installed IOSs
	u32 TitleCount = 0;
	ES_GetNumTitles(&TitleCount);
	u64 *TitleIDs=(u64*)memalign(32, TitleCount * sizeof(u64) );
	ES_GetTitles(TitleIDs, TitleCount);
	shellsort(TitleIDs, TitleCount);

	//get ios
	unsigned int IOS_off=0;
	if( SGetSetting(SETTING_SYSTEMMENUIOS) == 0 )
	{
		for( unsigned int i=0; i < TitleCount; ++i)
		{
			if( (u32)(TitleIDs[i]>>32) != 0x00000001 )
				continue;

			if( GetSysMenuIOS() == (u32)(TitleIDs[i]&0xFFFFFFFF) )
			{
				IOS_off=i;
				break;
			}
		}

	} else {

		for( unsigned int i=0; i < TitleCount; ++i)
		{
			if( (u32)(TitleIDs[i]>>32) != 0x00000001 )
				continue;

			if( SGetSetting(SETTING_SYSTEMMENUIOS) == (u32)(TitleIDs[i]&0xFFFFFFFF) )
			{
				IOS_off=i;
				break;
			}
		}
	}


	int cur_off=0;
	int redraw=true;
	while(1)
	{
		WPAD_ScanPads();
		PAD_ScanPads();

		u32 WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
		u32 PAD_Pressed  = PAD_ButtonsDown(0)  | PAD_ButtonsDown(1)  | PAD_ButtonsDown(2)  | PAD_ButtonsDown(3);

#ifdef DEBUG
		if ( (WPAD_Pressed & WPAD_BUTTON_HOME) || (PAD_Pressed & PAD_BUTTON_START) )
		{
			exit(0);
		}
#endif
		if ( WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B )
		{
			LoadSettings();
			SetShowDebug(SGetSetting(SETTING_SHOWGECKOTEXT));
			break;
		}
		switch( cur_off )
		{
			case 0:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT || WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT || PAD_Pressed & PAD_BUTTON_LEFT )
				{
					if( settings->autoboot == AUTOBOOT_DISABLED )
						settings->autoboot = AUTOBOOT_FILE;
					else
						settings->autoboot--;
					redraw=true;
				}else if ( WPAD_Pressed & WPAD_BUTTON_RIGHT || WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT || PAD_Pressed & PAD_BUTTON_RIGHT )
				{
					if( settings->autoboot == AUTOBOOT_FILE )
						settings->autoboot = AUTOBOOT_DISABLED;
					else
						settings->autoboot++;
					redraw=true;
				}
			} break;
			case 1:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					settings->ReturnTo++;
					if( settings->ReturnTo > RETURNTO_AUTOBOOT )
						settings->ReturnTo = RETURNTO_SYSMENU;

					redraw=true;
				} else if ( WPAD_Pressed & WPAD_BUTTON_LEFT || WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT || PAD_Pressed & PAD_BUTTON_LEFT ) {

					if( settings->ReturnTo == RETURNTO_SYSMENU )
						settings->ReturnTo = RETURNTO_AUTOBOOT;
					else
						settings->ReturnTo--;

					redraw=true;
				}


			} break;
			case 2:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if( settings->ShutdownToPreloader )
						settings->ShutdownToPreloader = 0;
					else 
						settings->ShutdownToPreloader = 1;

					redraw=true;
				}


			} break;
			case 3:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if( settings->StopDisc )
						settings->StopDisc = 0;
					else 
						settings->StopDisc = 1;

					redraw=true;
				}

			} break;
			case 4:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if( settings->LidSlotOnError )
						settings->LidSlotOnError = 0;
					else 
						settings->LidSlotOnError = 1;
				
					redraw=true;
				}


			} break;
			case 5:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if( settings->IgnoreShutDownMode )
						settings->IgnoreShutDownMode = 0;
					else 
						settings->IgnoreShutDownMode = 1;
				
					redraw=true;
				}


			} break;
			case 6:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if( settings->BlackBackground )
					{
						settings->BlackBackground = false;
					}
					else
					{
						settings->BlackBackground = true;
					}
					ClearScreen();
					redraw=true;
				}
			}
			break;
			case 7:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if( settings->PasscheckPriiloader )
					{
						settings->PasscheckPriiloader = false;
					}
					else
					{
						ClearScreen();
						PrintFormat( 1, ((rmode->viWidth /2)-((strlen("!!!!!WARNING!!!!!"))*13/2))>>1, 208, "!!!!!WARNING!!!!!");
						PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Setting Password can lock you out"))*13/2))>>1, 228, "Setting Password can lock you out" );
						PrintFormat( 1, ((rmode->viWidth /2)-((strlen("off your own wii. proceed? (A = Yes, B = No)"))*13/2))>>1, 248, "off your own wii. proceed? (A = Yes, B = No)" );
						while(1)
						{
							WPAD_ScanPads();
							PAD_ScanPads();
							u32 WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
							u32 PAD_Pressed  = PAD_ButtonsDown(0)  | PAD_ButtonsDown(1)  | PAD_ButtonsDown(2)  | PAD_ButtonsDown(3);
							if(WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A)
							{
								settings->PasscheckPriiloader = true;
								break;
							}
							else if(WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B)
							{
								break;
							}
						}
						ClearScreen();
						
					}
					redraw=true;
				}
			}
			break;
			case 8:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if( settings->PasscheckMenu )
					{
						settings->PasscheckMenu = false;
					}
					else
					{
						ClearScreen();
						PrintFormat( 1, ((rmode->viWidth /2)-((strlen("!!!!!WARNING!!!!!"))*13/2))>>1, 208, "!!!!!WARNING!!!!!");
						PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Setting Password can lock you out"))*13/2))>>1, 228, "Setting Password can lock you out" );
						PrintFormat( 1, ((rmode->viWidth /2)-((strlen("off your own wii. proceed? (A = Yes, B = No)"))*13/2))>>1, 248, "off your own wii. proceed? (A = Yes, B = No)" );
						while(1)
						{
							WPAD_ScanPads();
							PAD_ScanPads();
							u32 WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
							u32 PAD_Pressed  = PAD_ButtonsDown(0)  | PAD_ButtonsDown(1)  | PAD_ButtonsDown(2)  | PAD_ButtonsDown(3);
							if(WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A)
							{
								settings->PasscheckMenu = true;
								break;
							}
							else if(WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B)
							{
								break;
							}
						}
						ClearScreen();
					}
					redraw=true;
				}
			}
			break;
			case 9: //show Debug Info
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if ( settings->ShowGeckoText )
						settings->ShowGeckoText = 0;			
					else
						settings->ShowGeckoText = 1;
					SetShowDebug(settings->ShowGeckoText);
					redraw=true;
				}
			break;
			case 10: //download beta updates
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if ( settings->ShowBetaUpdates )
						settings->ShowBetaUpdates = 0;
					else
						settings->ShowBetaUpdates = 1;
					redraw=true;
				}
			break;		
			case 11: // Use Classic hacks file
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if( settings->UseClassicHacks )
					{
						settings->UseClassicHacks = false;
					}
					else
					{
						settings->UseClassicHacks = true;
					}
					ClearScreen();
					redraw=true;
				}
			break;
			case 12: //ignore ios reloading for system menu?
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT			||
					 PAD_Pressed & PAD_BUTTON_LEFT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT	|| 
					 WPAD_Pressed & WPAD_BUTTON_RIGHT			||
					 PAD_Pressed & PAD_BUTTON_RIGHT				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT	|| 
					 WPAD_Pressed & WPAD_BUTTON_A				||
					 WPAD_Pressed & WPAD_CLASSIC_BUTTON_A		|| 
					 PAD_Pressed & PAD_BUTTON_A
					)
				{
					if( settings->UseSystemMenuIOS )
					{
						settings->UseSystemMenuIOS = false;
						if(settings->SystemMenuIOS == 0)
						{
							while( (u32)(TitleIDs[IOS_off]&0xFFFFFFFF) < 3  || (u32)(TitleIDs[IOS_off]&0xFFFFFFFF) > 256 )
							{
								if( (u32)(TitleIDs[IOS_off]&0xFFFFFFFF) > 256 )
									IOS_off--;
								else
									IOS_off++;
							}
							settings->SystemMenuIOS = (u32)(TitleIDs[IOS_off]&0xFFFFFFFF);
						}
					}
					else
					{
						settings->UseSystemMenuIOS = true;
					}
					redraw=true;
				}
			}
			break;
			case 13:		//	System Menu IOS
			{
				if ( WPAD_Pressed & WPAD_BUTTON_LEFT || WPAD_Pressed & WPAD_CLASSIC_BUTTON_LEFT || PAD_Pressed & PAD_BUTTON_LEFT )
				{
					while(1)
					{
						IOS_off--;
						if( (signed)IOS_off <= 0 )
							IOS_off = TitleCount;
						if( (u32)(TitleIDs[IOS_off]>>32) == 0x00000001 && (u32)(TitleIDs[IOS_off]&0xFFFFFFFF) > 2 && (u32)(TitleIDs[IOS_off]&0xFFFFFFFF) < 255 )
							break;
					}

					settings->SystemMenuIOS = (u32)(TitleIDs[IOS_off]&0xFFFFFFFF);
#ifdef DEBUG
					isIOSstub(settings->SystemMenuIOS);
#endif

					redraw=true;
				} else if( WPAD_Pressed & WPAD_BUTTON_RIGHT || WPAD_Pressed & WPAD_CLASSIC_BUTTON_RIGHT || PAD_Pressed & PAD_BUTTON_RIGHT ) 
				{
					while(1)
					{
						IOS_off++;
						if( IOS_off >= TitleCount )
							IOS_off = 2;
						if( (u32)(TitleIDs[IOS_off]>>32) == 0x00000001 && (u32)(TitleIDs[IOS_off]&0xFFFFFFFF) > 2  && (u32)(TitleIDs[IOS_off]&0xFFFFFFFF) < 255 )
							break;
					}

					settings->SystemMenuIOS = (u32)(TitleIDs[IOS_off]&0xFFFFFFFF);
#ifdef DEBUG
					isIOSstub(settings->SystemMenuIOS);
#endif
					redraw=true;
				}

			} break;
			case 14:
			{
				if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
				{
					if( SaveSettings() )
						PrintFormat( 0, 114, 128+224+16, "settings saved");
					else
						PrintFormat( 0, 118, 128+224+16, "saving failed");
				}
			} break;

			default:
				cur_off = 0;
				break;
		}

		if ( WPAD_Pressed & WPAD_BUTTON_DOWN || WPAD_Pressed & WPAD_CLASSIC_BUTTON_DOWN || PAD_Pressed & PAD_BUTTON_DOWN )
		{
			cur_off++;
			if( (settings->UseSystemMenuIOS) && (cur_off == 13))
				cur_off++;
			if( cur_off >= 15)
				cur_off = 0;
			
			redraw=true;
		} else if ( WPAD_Pressed & WPAD_BUTTON_UP || WPAD_Pressed & WPAD_CLASSIC_BUTTON_UP || PAD_Pressed & PAD_BUTTON_UP )
		{
			cur_off--;
			if( (settings->UseSystemMenuIOS) && (cur_off == 13))
				cur_off--;
			if( cur_off < 0 )
				cur_off = 14;
			
			redraw=true;
		}
		if( redraw )
		{
			switch(settings->autoboot)
			{
				case AUTOBOOT_DISABLED:
					PrintFormat( cur_off==0, 0, 112,    "              Autoboot:          Disabled        ");
				break;

				case AUTOBOOT_SYS:
					PrintFormat( cur_off==0, 0, 112,    "              Autoboot:          System Menu     ");
				break;
				case AUTOBOOT_HBC:
					PrintFormat( cur_off==0, 0, 112,    "              Autoboot:          Homebrew Channel");
				break;

				case AUTOBOOT_BOOTMII_IOS:
					PrintFormat( cur_off==0, 0, 112,    "              Autoboot:          BootMii IOS     ");
				break;

				case AUTOBOOT_FILE:
					PrintFormat( cur_off==0, 0, 112,    "              Autoboot:          Installed File  ");
				break;
				default:
					settings->autoboot = AUTOBOOT_DISABLED;
					break;
			}

			switch( settings->ReturnTo )
			{
				case RETURNTO_SYSMENU:
					PrintFormat( cur_off==1, 0, 128,    "             Return to:          System Menu");
				break;
				case RETURNTO_PRELOADER:
					PrintFormat( cur_off==1, 0, 128,    "             Return to:          Priiloader  ");
				break;
				case RETURNTO_AUTOBOOT:
					PrintFormat( cur_off==1, 0, 128,    "             Return to:          Autoboot   ");
				break;
				default:
					gprintf("SetSettings : unknown return to value %d\n",settings->ReturnTo);
			}
			
			//PrintFormat( 0, 16, 64, "Pos:%d", ((rmode->viWidth /2)-(strlen("settings saved")*13/2))>>1);

			PrintFormat( cur_off==2, 0, 128+16, "           Shutdown to:          %s", settings->ShutdownToPreloader?"Priiloader":"off       ");
			PrintFormat( cur_off==3, 0, 128+32, "  Stop disc on startup:          %s", settings->StopDisc?"on ":"off");
			PrintFormat( cur_off==4, 0, 128+48, "   Light slot on error:          %s", settings->LidSlotOnError?"on ":"off");
			PrintFormat( cur_off==5, 0, 128+64, "        Ignore standby:          %s", settings->IgnoreShutDownMode?"on ":"off");
			PrintFormat( cur_off==6, 0, 128+80, "      Background Color:          %s", settings->BlackBackground?"Black":"White");
			PrintFormat( cur_off==7, 0, 128+96, "    Protect Priiloader:          %s", settings->PasscheckPriiloader?"on ":"off");
			PrintFormat( cur_off==8, 0, 128+112,"      Protect Autoboot:          %s", settings->PasscheckMenu?"on ":"off");
			PrintFormat( cur_off==9, 0, 128+128,"   Display Gecko ouput:          %s", settings->ShowGeckoText?"on ":"off");
			PrintFormat( cur_off==10,0, 128+144,"     Show Beta Updates:          %s", settings->ShowBetaUpdates?"on ":"off");
			PrintFormat( cur_off==11,0, 128+160," Use Classic Hacks.ini:          %s", settings->UseClassicHacks?"on ":"off");
			PrintFormat( cur_off==12,0, 128+176,"   Use System Menu IOS:          %s", settings->UseSystemMenuIOS?"on ":"off");
			if(!settings->UseSystemMenuIOS)
			{
				PrintFormat( cur_off==13, 0, 128+176, "     IOS to use for SM:          %d  ", (u32)(TitleIDs[IOS_off]&0xFFFFFFFF) );
			}
			else
			{
				PrintFormat( cur_off==13, 0, 128+192,	"                                        ");
			}
			PrintFormat( cur_off==14, 118, 128+224, "save settings");
			PrintFormat( 0, 114, 128+224+16, "                 ");

			redraw = false;
		}

		VIDEO_WaitVSync();
	}
	free_pointer(TitleIDs);
	return;
}
void LoadHBC( void )
{
	//Note By DacoTaco :check for (0x00010001/4A4F4449 - JODI) HBC id
	//or old one(0x00010001/148415858 - HAXX)
	//or latest 0x00010001/AF1BF516
	u64 TitleID = 0;
	switch (DetectHBC())
	{
		case 1: //HAXX
			gprintf("LoadHBC : HAXX detected\n");
			TitleID = 0x0001000148415858LL;
			break;
		case 2: //JODI
			gprintf("LoadHBC : JODI detected\n");
			TitleID = 0x000100014A4F4449LL;
			break;
		case 3: //0.7
			gprintf("LoadHBC : 0.7 HBC detected\n");
			TitleID = 0x00010001AF1BF516LL;
			break;
		default: //LOL nothing?
			error = ERROR_BOOT_HBC;
			return;
	}
	u32 cnt ATTRIBUTE_ALIGN(32);
	ES_GetNumTicketViews(TitleID, &cnt);
	tikview *views = (tikview *)memalign( 32, sizeof(tikview)*cnt );
	ES_GetTicketViews(TitleID, views, cnt);
	if( ClearState() < 0 )
	{
		gprintf("ClearState failure\n");
	}
	ES_LaunchTitle(TitleID, &views[0]);
	//well that went wrong
	error = ERROR_BOOT_HBC;
	free_pointer(views);
	return;
}
void LoadBootMii( void )
{
	//when this was coded on 6th of Oct 2009 Bootmii ios was in IOS slot 254
	if(isIOSstub(254))
	{
		if(rmode != NULL)
		{
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Bootmii(IOS254) Not found!"))*13/2))>>1, 208, "Bootmii(IOS254) Not found!");
			sleep(5);
		}
		return;
	}
	if ( !(Mounted & 2) )
	{
		if(rmode != NULL)
		{
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Could not mount SD card"))*13/2))>>1, 208, "Could not mount SD card");
			sleep(5);
		}
		return;
	}
	FILE* BootmiiFile = fopen("fat:/bootmii/armboot.bin","r");
	if (!BootmiiFile)
	{
		if(rmode != NULL)
		{
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Could not find fat:/bootmii/armboot.bin"))*13/2))>>1, 208, "Could not find fat:/bootmii/armboot.bin");
			sleep(5);
		}
		return;
	}
	else
	{
		fclose(BootmiiFile);
		BootmiiFile = fopen("fat:/bootmii/ppcboot.elf","r");
		if(!BootmiiFile)
		{
			if(rmode != NULL)
			{	
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Could not find fat:/bootmii/ppcboot.elf"))*13/2))>>1, 208, "Could not find fat:/bootmii/ppcboot.elf");
				sleep(5);
			}
			return;
		}
	}
	fclose(BootmiiFile);
	u8 currentIOS = IOS_GetVersion();
	for(u8 i=0;i<WPAD_MAX_WIIMOTES;i++) {
		WPAD_Flush(i);
		WPAD_Disconnect(i);
	}
	WPAD_Shutdown();
	//clear the bootstate before going on
	if( ClearState() < 0 )
	{
		gprintf("ClearState failure\n");
	}
	IOS_ReloadIOS(254);
	//launching bootmii failed. lets wait a bit for the launch(it could be delayed) and then load the other ios back
	sleep(5);
	IOS_ReloadIOS(currentIOS);
	ReloadedIOS = 1;
	WPAD_Init();
	return;
}
void DVDStopDisc( bool do_async )
{
	s32 di_fd = IOS_Open("/dev/di",0);
	if(di_fd)
	{
		u8 *inbuf = (u8*)memalign( 32, 0x20 );
		u8 *outbuf = (u8*)memalign( 32, 0x20 );

		memset(inbuf, 0, 0x20 );
		memset(outbuf, 0, 0x20 );

		((u32*)inbuf)[0x00] = 0xE3000000;
		((u32*)inbuf)[0x01] = 0;
		((u32*)inbuf)[0x02] = 0;

		DCFlushRange(inbuf, 0x20);
		//why crediar used an async is beyond me but i looks wrong -for a shutdown-... :/
		if(!do_async)
			IOS_Ioctl( di_fd, 0xE3, inbuf, 0x20, outbuf, 0x20);
		else
		{
			IOS_IoctlAsync( di_fd, 0xE3, inbuf, 0x20, outbuf, 0x20, NULL, NULL);
		}

		free_pointer( outbuf );
		free_pointer( inbuf );
	}
	else
		gprintf("DVDStopDisc : IOS_Open error %d\n",di_fd);
}
s8 BootDolFromMem( u8 *dolstart ) 
{
	if(dolstart == NULL)
		return -1;

    u32 i;
	void	(*entrypoint)();
	
	Elf32_Ehdr ElfHdr;

	memcpy(&ElfHdr,dolstart,sizeof(Elf32_Ehdr));

	if( ElfHdr.e_ident[EI_MAG0] == 0x7F ||
		ElfHdr.e_ident[EI_MAG1] == 'E' ||
		ElfHdr.e_ident[EI_MAG2] == 'L' ||
		ElfHdr.e_ident[EI_MAG3] == 'F' )
	{
#ifdef DEBUG
		gprintf("BootDolFromMem : ELF Found\n");
		gprintf("Type:      \t%04X\n", ElfHdr.e_type );
		gprintf("Machine:   \t%04X\n", ElfHdr.e_machine );
		gprintf("Version:  %08X\n", ElfHdr.e_version );
		gprintf("Entry:    %08X\n", ElfHdr.e_entry );
		gprintf("Flags:    %08X\n", ElfHdr.e_flags );
		gprintf("EHsize:    \t%04X\n\n", ElfHdr.e_ehsize );

		gprintf("PHoff:    %08X\n",	ElfHdr.e_phoff );
		gprintf("PHentsize: \t%04X\n",	ElfHdr.e_phentsize );
		gprintf("PHnum:     \t%04X\n\n",ElfHdr.e_phnum );

		gprintf("SHoff:    %08X\n",	ElfHdr.e_shoff );
		gprintf("SHentsize: \t%04X\n",	ElfHdr.e_shentsize );
		gprintf("SHnum:     \t%04X\n",	ElfHdr.e_shnum );
		gprintf("SHstrndx:  \t%04X\n\n",ElfHdr.e_shstrndx );
#endif
		if( ElfHdr.e_phnum == 0 )
		{
#ifdef DEBUG
			gprintf("BootDolFromMem : Warning program header entries are zero!\n");
#endif
		} 
		else 
		{
			for( s32 i=0; i < ElfHdr.e_phnum; ++i )
			{
				Elf32_Phdr phdr;
				ICInvalidateRange (&phdr ,sizeof( phdr ) );
				memmove(&phdr,dolstart + (ElfHdr.e_phoff + sizeof( Elf32_Phdr ) * i) ,sizeof( phdr ) );
#ifdef DEBUG
				gprintf("Type:%08X Offset:%08X VAdr:%08X PAdr:%08X FileSz:%08X\n", phdr.p_type, phdr.p_offset, phdr.p_vaddr, phdr.p_paddr, phdr.p_filesz );
#endif
				ICInvalidateRange ((void*)(phdr.p_vaddr | 0x80000000),phdr.p_filesz);
				if(phdr.p_type == PT_LOAD )
					memmove((void*)(phdr.p_vaddr | 0x80000000), dolstart + phdr.p_offset , phdr.p_filesz);
			}
		}

		//according to dhewg the section headers are totally un-needed (infact, they break a few elf loading)
		//however, checking for the type does the trick to make them work :)
		if( ElfHdr.e_shnum == 0 )
		{
#ifdef DEBUG
			gprintf("BootDolFromMem : Warning section header entries are zero!\n");
#endif
		} 
		else 
		{

			for( s32 i=0; i < ElfHdr.e_shnum; ++i )
			{

				Elf32_Shdr shdr;
				memmove(&shdr, dolstart + (ElfHdr.e_shoff + sizeof( Elf32_Shdr ) * i) ,sizeof( shdr ) );
				DCFlushRangeNoSync(&shdr ,sizeof( shdr ) );

				if( shdr.sh_type == SHT_NULL )
					continue;

#ifdef DEBUG
				if( shdr.sh_type > SHT_GROUP )
					gprintf("Warning the type: %08X could be invalid!\n", shdr.sh_type );

				if( shdr.sh_flags & ~0xF0000007 )
					gprintf("Warning the flag: %08X is invalid!\n", shdr.sh_flags );

				gprintf("Type:%08X Offset:%08X Name:%08X Off:%08X Size:%08X\n", shdr.sh_type, shdr.sh_offset, shdr.sh_name, shdr.sh_addr, shdr.sh_size );
#endif
				if (shdr.sh_type == SHT_NOBITS)
				{
					memmove((void*)(shdr.sh_addr | 0x80000000), dolstart + shdr.sh_offset,shdr.sh_size);
					DCFlushRangeNoSync((void*)(shdr.sh_addr | 0x80000000),shdr.sh_size);
				}
			}
		}
		entrypoint = (void (*)())(ElfHdr.e_entry | 0x80000000);
	}
	else
	{
#ifdef DEBUG
		gprintf("BootDolFromMem : DOL detected\n");
#endif
		dolhdr *dolfile;
		dolfile = (dolhdr *) dolstart;
		for (i = 0; i < 7; i++) {
			if ((!dolfile->sizeText[i]) || (dolfile->addressText[i] < 0x100)) continue;
			ICInvalidateRange ((void *) dolfile->addressText[i],dolfile->sizeText[i]);
			memmove ((void *) dolfile->addressText[i],dolstart+dolfile->offsetText[i],dolfile->sizeText[i]);
		}
#ifdef DEBUG
		gprintf("Data Sections :\n");
#endif
		for (i = 0; i < 11; i++) {
			if ((!dolfile->sizeData[i]) || (dolfile->offsetData[i] < 0x100)) continue;
			memmove ((void *) dolfile->addressData[i],dolstart+dolfile->offsetData[i],dolfile->sizeData[i]);
			DCFlushRangeNoSync ((void *) dolfile->offsetData[i],dolfile->sizeData[i]);
#ifdef DEBUG
			gprintf("\t%08x\t\t%08x\t\t%08x\t\t\n", (dolfile->offsetData[i]), dolfile->addressData[i], dolfile->sizeData[i]);
#endif
		}

		memset ((void *) dolfile->addressBSS, 0, dolfile->sizeBSS);
		DCFlushRange((void *) dolfile->addressBSS, dolfile->sizeBSS);
		entrypoint = (void (*)())(dolfile->entrypoint);
	}
	if(entrypoint == 0x00000000 )
	{
		gprintf("BootDolFromMem : bogus entrypoint of %08X detected\n",(u32)(entrypoint));
		return -2;
	}
	gprintf("BootDolFromMem : starting binary...\n");
	for (int i = 0;i < WPAD_MAX_WIIMOTES ;i++)
	{
		if(WPAD_Probe(i,0) > 0)
		{
			WPAD_Flush(i);
			WPAD_Disconnect(i);
		}
	}
	ClearState();
	WPAD_Shutdown();
	ShutdownDevices();
	DVDStopDisc(false);

	if(ReloadedIOS)
	{
		if( isIOSstub(58) )
		{
			if( isIOSstub(IOS_GetPreferredVersion()) )
			{
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("failed to reload ios for homebrew! ios is a stub!"))*13/2))>>1, 208, "failed to reload ios for homebrew! ios is a stub!");
				sleep(3);
			}
			else
			{
				IOS_ReloadIOS(IOS_GetPreferredVersion());
				ReloadedIOS = 1;
			}
		}
		else
		{
			IOS_ReloadIOS(58);
			ReloadedIOS = 1;
		}
	}
	
	gprintf("BootDolFromMem : Entrypoint: %08X\n", (u32)(entrypoint) );

	__STM_Close();
	ISFS_Deinitialize();
	ShutdownDevices();
	USB_Deinitialize();
	__IOS_ShutdownSubsystems();
	mtmsr(mfmsr() & ~0x8000);
	mtmsr(mfmsr() | 0x2002);
	ICSync();
	entrypoint();
	/*
	//alternate booting code. seems to be as good(or bad) as the above code
	u32 level;
	__STM_Close();
	ISFS_Deinitialize();
	ShutdownDevices();
	USB_Deinitialize();
	__IOS_ShutdownSubsystems();
	SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
    _CPU_ISR_Disable (level);
	mtmsr(mfmsr() & ~0x8000);
	mtmsr(mfmsr() | 0x2002);
	ICSync();
    entrypoint();
    _CPU_ISR_Restore (level);*/

	//it failed. FAIL!
	__IOS_InitializeSubsystems();
	PollDevices();
	ISFS_Initialize();
	WPAD_Init();
	PAD_Init();
	gprintf("BootDolFromMem : booting failure\n");
	return -1;
}

s8 BootDolFromDir( const char* Dir )
{
	if (Mounted == 0)
	{
		return -1;
	}
	FILE* dol;
	s32 lSize;
	u8* buffer;
	dol = fopen(Dir,"rb");
	gprintf("BootDolFromDir : loading %s\n",Dir);
	if (dol)
	{
		// obtain file size:
		fseek (dol , 0 , SEEK_END);
		lSize = ftell (dol);
		rewind (dol);
		// allocate memory to contain the whole file:
		buffer = (u8*) memalign (32,(sizeof(u8)*lSize+31)&(~31));
		if (buffer == NULL) 
		{
			return -3;
		}
		else
		{
			// copy the file into the buffer:
			s32 result = fread (buffer,1,lSize,dol);
			if (result != lSize) 
			{
				gprintf("BootDolFromDir : fread failure\n");
				free_pointer(buffer);
				return -4;
			}
			else
			{
				BootDolFromMem(buffer);
				free_pointer(buffer);
				return -5;
			}
		}
	}
	else
	{
		gprintf("BootDolFromDir : fopen(%s) fail\n",Dir);
		return -2;
	}
}
void BootMainSysMenu( u8 init )
{
	//memory block variables used within the function:
	//ticket stuff:
	char * buf = NULL;
	fstats * tstatus = NULL;

	//TMDview stuff:
	u64 TitleID=0x0000000100000002LL;
	u32 tmd_size;
	tmd_view *rTMD = NULL;

	//TMD:
	signed_blob *TMD = NULL;

	//boot file:
	u32 fileID = 0;
	char file[265] ATTRIBUTE_ALIGN(32);
	fstats *status = NULL;
	dolhdr *boot_hdr = NULL;

	//hacks : hashes
	u8* mem_block = 0;
	u32 max_address = 0;


	//general:
	s32 r = 0;
	s32 fd = 0;


	if(init == 0)
	{
		PollDevices();
	}
	//booting sys menu
	ISFS_Deinitialize();
	if( ISFS_Initialize() < 0 )
	{
		if (rmode != NULL)
		{
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("ISFS_Initialize() failed!"))*13/2))>>1, 208, "ISFS_Initialize() failed!" );
			sleep( 5 );
		}
		return;
	}
	
	//expermintal code for getting the needed tmd info. no boot index is in the views but lunatik and i think last file = boot file
	r = ES_GetTMDViewSize(TitleID, &tmd_size);
	if(r<0)
	{
		gprintf("GetTMDViewSize error %d\n",r);
		error = ERROR_SYSMENU_GETTMDSIZEFAILED;
		goto free_and_return;
	}
	rTMD = (tmd_view*)memalign( 32, (tmd_size+31)&(~31) );
	if( rTMD == NULL )
	{
		error = ERROR_MALLOC;
		goto free_and_return;
	}
	memset(rTMD,0, (tmd_size+31)&(~31) );
	r = ES_GetTMDView(TitleID, (u8*)rTMD, tmd_size);
	if(r<0)
	{
		gprintf("GetTMDView error %d\n",r);
		error = ERROR_SYSMENU_GETTMDFAILED;
		goto free_and_return;
	}
#ifdef DEBUG
	gprintf("ios version: %u\n",(u8)rTMD->sys_version);
#endif

	//get main.dol filename
	/*for(u32 z=0; z < rTMD->num_contents; ++z)
	{
		if( rTMD->contents[z].index == rTMD->num_contents )//rTMD->boot_index )
		{
#ifdef DEBUG
			gprintf("content[%i] id=%08X type=%u\n", z, content->cid, content->type | 0x8001 );
#endif
			fileID = rTMD->contents[z].cid;
			break;
		}
	}*/
	fileID = rTMD->contents[rTMD->num_contents-1].cid;
	gprintf("using %08X for booting\n",rTMD->contents[rTMD->num_contents-1].cid);

	if( fileID == 0 )
	{
		error = ERROR_SYSMENU_BOOTNOTFOUND;
		goto free_and_return;
	}

	sprintf( file, "/title/00000001/00000002/content/%08x.app", fileID );
	//small fix that Phpgeek didn't forget but i did
	file[33] = '1'; // installing preloader renamed system menu so we change the app file to have the right name

	fd = ISFS_Open( file, 1 );
#ifdef DEBUG
	gprintf("ISFS_Open(%s, %d):%d\n", file, 1, fd );
	sleep(1);
#endif
	if( fd < 0 )
	{
		gprintf("error opening %08x.app! error %d\n",fileID,fd);
		ISFS_Close( fd );
		error = ERROR_SYSMENU_BOOTOPENFAILED;
		goto free_and_return;
	}

	status = (fstats *)memalign(32, (sizeof( fstats )+31)&(~31) );
	if( status == NULL )
	{
		ISFS_Close( fd );
		error = ERROR_MALLOC;
		goto free_and_return;
	}
	memset(status,0, (sizeof( fstats )+31)&(~31) );
	r = ISFS_GetFileStats( fd, status);
#ifdef DEBUG
	printf("ISFS_GetFileStats(%d, %08X):%d\n", fd, status, r );
	sleep(1);
#endif
	if( r < 0 || status->file_length == 0)
	{
		ISFS_Close( fd );
		error = ERROR_SYSMENU_BOOTGETSTATS;
		free_pointer(status);
		goto free_and_return;
	}
#ifdef DEBUG
	printf("size:%d\n", status->file_length);
#endif
	free_pointer(status);
	boot_hdr = (dolhdr *)memalign(32, (sizeof( dolhdr )+31)&(~31) );
	if(boot_hdr == NULL)
	{
		gprintf("memalign failure(dolhdr)\n");
		error = ERROR_MALLOC;
		ISFS_Close(fd);
		goto free_and_return;
	}
	memset( boot_hdr, 0, (sizeof( dolhdr )+31)&(~31) );
	
	r = ISFS_Seek( fd, 0, SEEK_SET );
	if ( r < 0)
	{
		gprintf("ISFS_Seek error %d(dolhdr)\n",r);
		ISFS_Close(fd);
		goto free_and_return;
	}
	r = ISFS_Read( fd, boot_hdr, sizeof(dolhdr) );
#ifdef DEBUG
	printf("ISFS_Read(%d, %08X, %d):%d\n", fd, hdr, sizeof(dolhdr), r );
	sleep(1);
#endif

	if( r < 0 || r != sizeof(dolhdr) )
	{
		gprintf("ISFS_Read error %d of dolhdr\n",r);
		ISFS_Close( fd );
		goto free_and_return;
	}
	if( boot_hdr->entrypoint != 0x3400 )
	{
		gprintf("Bogus Entrypoint detected!\n");
		ISFS_Close( fd );
		goto free_and_return;
	}

	void	(*entrypoint)();
	for (u8 i = 0; i < 6; i++)
	{
		if( boot_hdr->sizeText[i] && boot_hdr->addressText[i] && boot_hdr->offsetText[i] )
		{
			ICInvalidateRange((void*)(boot_hdr->addressText[i]), boot_hdr->sizeText[i]);
#ifdef DEBUG
			gprintf("\t%08x\t\t%08x\t\t%08x\t\t\n", (boot_hdr->offsetText[i]), boot_hdr->addressText[i], boot_hdr->sizeText[i]);
#endif
			if( (((boot_hdr->addressText[i])&0xF0000000) != 0x80000000) || (boot_hdr->sizeText[i]>(10*1024*1024)) )
			{
				gprintf("bogus Text offset\n");
				ISFS_Close( fd );
				goto free_and_return;
			}

			r = ISFS_Seek( fd, boot_hdr->offsetText[i], SEEK_SET );
			if ( r < 0)
			{
				gprintf("ISFS_Seek error %d(offsetText)\n");
				ISFS_Close(fd);
				goto free_and_return;
			}
			ISFS_Read( fd, (void*)(boot_hdr->addressText[i]), boot_hdr->sizeText[i] );

			DCFlushRange((void*)(boot_hdr->addressText[i]), boot_hdr->sizeText[i]);
		}
	}
	// data sections
	for (u8 i = 0; i <= 10; i++)
	{
		if( boot_hdr->sizeData[i] && boot_hdr->addressData[i] && boot_hdr->offsetData[i] )
		{
			ICInvalidateRange((void*)(boot_hdr->addressData[i]), boot_hdr->sizeData[i]);
#ifdef DEBUG
			gprintf("\t%08x\t\t%08x\t\t%08x\t\t\n", (hdr->offsetData[i]), hdr->addressData[i], hdr->sizeData[i]);
#endif
			if( (((boot_hdr->addressData[i])&0xF0000000) != 0x80000000) || (boot_hdr->sizeData[i]>(10*1024*1024)) )
			{
				gprintf("bogus Data offsets\n");
				ISFS_Close(fd);
				goto free_and_return;
			}

			r = ISFS_Seek( fd, boot_hdr->offsetData[i], SEEK_SET );
			if ( r < 0)
			{
				gprintf("ISFS_Seek error %d(offsetData)\n");
				ISFS_Close(fd);
				goto free_and_return;
			}
			r = ISFS_Read( fd, (void*)boot_hdr->addressData[i], boot_hdr->sizeData[i] );
			if (r < 0)
			{
				gprintf("ISFS_Read error %d(addressdata)\n",r);
				ISFS_Close(fd);
				goto free_and_return;
			}
			DCFlushRange((void*)boot_hdr->addressData[i], boot_hdr->sizeData[i]);
		}

	}
	entrypoint = (void (*)())(boot_hdr->entrypoint);
	gprintf("entrypoint %08X\n", entrypoint );

	if( !SGetSetting( SETTING_CLASSIC_HACKS ) )
	{
		LoadHacks_Hash(true);
	}
	else
	{
		LoadHacks(true);
	}
	for(u8 i=0;i<WPAD_MAX_WIIMOTES;i++) {
		WPAD_Flush(i);
		WPAD_Disconnect(i);
	}
	WPAD_Shutdown();

	//Step 1 of IOS handling : Reloading IOS if needed;
	if( !SGetSetting( SETTING_USESYSTEMMENUIOS ) )
	{
		s32 ToLoadIOS = SGetSetting(SETTING_SYSTEMMENUIOS);
		if ( ToLoadIOS != (u8)IOS_GetVersion() )
		{
			gprintf("BootMainSysMenu : IsIOSStub(%d)...\n",ToLoadIOS);
			if ( !isIOSstub(ToLoadIOS) )
			{
				__ES_Close();
				__IOS_ShutdownSubsystems();
				__ES_Init();
				__IOS_LaunchNewIOS ( ToLoadIOS );
				//why the hell the es needs 2 init's is beyond me... it just happens (see IOS_ReloadIOS in libogc's ios.c)
				__ES_Init();
				gprintf("BootMainSysMenu : ios %d launched\n",IOS_GetVersion());
				//__IOS_LaunchNewIOS ( (u8)rTMD->sys_version );
				//__IOS_LaunchNewIOS ( 249 );
				ReloadedIOS = 1;
			}
			else
			{
				WPAD_Init();
				error=ERROR_SYSMENU_IOSSTUB;
				goto free_and_return;
			}
		}
		else
		{
			gprintf("skipping IOS reload\n");
		}
	}
	/*
	//technically its needed... but i fail to see why...
	else if ((u8)IOS_GetVersion() != (u8)rTMD->sys_version)
	{
		gprintf("Use system menu is ON, but IOS %d isn't loaded. reloading IOS...\n",(u8)rTMD->sys_version);
		__ES_Close();
		__IOS_ShutdownSubsystems();
		__ES_Init();
		__IOS_LaunchNewIOS ( (u8)rTMD->sys_version );
		__IOS_InitializeSubsystems();

		gprintf("launched ios %d for system menu\n",IOS_GetVersion());
		ReloadedIOS = 1;
	}*/
	//Step 2 of IOS handling : ES_Identify if we are on a different ios or if we reloaded ios once already. note that the ES_Identify is only supported by ios > 20
	if (((u8)IOS_GetVersion() != (u8)rTMD->sys_version) || (ReloadedIOS) )
	{
		if (ReloadedIOS)
			gprintf("Forced into ES_Identify\n");
		else
			gprintf("IOS(%d) != SM IOS(%d). forcing ES_Identify\n",IOS_GetVersion(),(u8)rTMD->sys_version);
		//read ticket from FS
		fd = ISFS_Open("/title/00000001/00000002/content/ticket", 1 );
		if( fd < 0 )
		{
			error = ERROR_SYSMENU_TIKNOTFOUND;
			goto free_and_return;
		}

		//get size
		tstatus = (fstats*)memalign( 32, sizeof( fstats ) );
		if(tstatus == NULL)
		{
			ISFS_Close( fd );
			error = ERROR_MALLOC;
			goto free_and_return;
		}
		r = ISFS_GetFileStats( fd, tstatus );
		if( r < 0 )
		{
			ISFS_Close( fd );
			error = ERROR_SYSMENU_TIKSIZEGETFAILED;
			goto free_and_return;
		}

		//create buffer
		buf = (char*)memalign( 32, (tstatus->file_length+31)&(~31) );
		if( buf == NULL )
		{
			ISFS_Close( fd );
			error = ERROR_MALLOC;
			goto free_and_return;
		}
		memset(buf, 0, (tstatus->file_length+31)&(~31) );

		//read file
		r = ISFS_Read( fd, buf, tstatus->file_length );
		if( r < 0 )
		{
			ISFS_Close( fd );
			error = ERROR_SYSMENU_TIKREADFAILED;
			goto free_and_return;
		}

		ISFS_Close( fd );
		//get the real TMD. we didn't get the real TMD before. the views will fail to be used in identification
		u32 tmd_size_temp;
		r=ES_GetStoredTMDSize(TitleID, &tmd_size_temp);
		if(r < 0)
		{
			error=ERROR_SYSMENU_ESDIVERFIY_FAILED;
			gprintf("ES_Identify: GetStoredTMDSize error %d\n",r);
			__IOS_InitializeSubsystems();
			WPAD_Init();
			goto free_and_return;
		}
		TMD = (signed_blob *)memalign( 32, (tmd_size_temp+31)&(~31) );
		if(TMD == NULL)
		{
			gprintf("ES_Identify: memalign TMD failure\n");
			error = ERROR_MALLOC;
			__IOS_InitializeSubsystems();
			WPAD_Init();
			goto free_and_return;
		}
		memset(TMD, 0, tmd_size_temp);

		r=ES_GetStoredTMD(TitleID, TMD, tmd_size_temp);
		if(r < 0)
		{
			error=ERROR_SYSMENU_ESDIVERFIY_FAILED;
			gprintf("ES_Identify: GetStoredTMD error %d\n",r);
			__IOS_InitializeSubsystems();
			WPAD_Init();
			goto free_and_return;
		}
		r = ES_Identify( (signed_blob *)certs_bin, certs_bin_size, (signed_blob *)TMD, tmd_size_temp, (signed_blob *)buf, tstatus->file_length, 0);
		if( r < 0 )
		{	
			error=ERROR_SYSMENU_ESDIVERFIY_FAILED;
			//__IOS_LaunchNewIOS ( (u8)rTMD->sys_version );
			gprintf("ES_Identify error %d\n",r);
			__IOS_InitializeSubsystems();
			WPAD_Init();
			goto free_and_return;
		}
	}
	ES_SetUID(TitleID);
	if(tstatus)
	{
		free_pointer( tstatus );
	}
	if(buf)
	{
		free_pointer( buf );
	}

	*(vu32*)0x800000F8 = 0x0E7BE2C0;				// Bus Clock Speed
	*(vu32*)0x800000FC = 0x2B73A840;				// CPU Clock Speed

	if( SGetSetting( SETTING_CLASSIC_HACKS ) )
	{
		gprintf("Hacks:%d\n", hacks.size() );
		//Apply patches
		for( u32 i=0; i<hacks.size(); ++i)
		{
	#ifdef DEBUG
			printf("i:%d state:%d version:%d\n", i, states[i], hacks[i].version);
	#endif
			if( states[i] == 1 )
			{
				if( hacks[i].version != rTMD->title_version )
					continue;

				for( u32 z=0; z < hacks[i].value.size(); ++z )
				{
	#ifdef DEBUG
					printf("%08X:%08X\n", hacks[i].offset[z], hacks[i].value[z] );
	#endif
					*(vu32*)(hacks[i].offset[z]) = hacks[i].value[z];
					DCFlushRange((void*)(hacks[i].offset[z]), 4);
				}
			}
		}
	}
	else
	{
		//TODO : either load & apply both methods or add a way of adding multiple addresses to the hash method...
		//add the offset/value method that defines extra writes to do after the hash?
		gprintf("Hacks:%d\n",hacks_hash.size());
		if(hacks_hash.size() != 0)
		{
			mem_block = (u8*)(*boot_hdr->addressData - *boot_hdr->offsetData);
			max_address = (u32)(*boot_hdr->sizeData + *boot_hdr->addressData);
			gprintf("mem_block range : 0x%08X - 0x%08X\n",mem_block,max_address);
			for(u32 i = 0;i < hacks_hash.size();i++)
			{
				if(states_hash[i] == 1)
				{
					u32 add = 0;
					for(u32 y = 0; y < hacks_hash[i].amount;y++)
					{
						//gprintf("y = %d\nsize of patch is %d\nwith as last u32 0x%08X\n",y,hacks_hash[i].patch[y].size(),hacks_hash[i].patch[y][hacks_hash[i].patch[y].size() -1]);
						while( add + (u32)mem_block < max_address)
						{
							u32 temp_hash[hacks_hash[i].patches[y].hash.size()];
							u32 temp_patch[hacks_hash[i].patches[y].patch.size()];
							for(u32 z = 0;z < hacks_hash[i].patches[y].hash.size(); z++)
							{
								temp_hash[z] = hacks_hash[i].patches[y].hash[z];
							}
							if ( !memcmp(mem_block+add, temp_hash ,sizeof(temp_hash)) )
							{
								gprintf("Found %s @ 0x%X, patching...\n",hacks_hash[i].desc.c_str(), add+(u32)mem_block);
								for(u32 z = 0;z < hacks_hash[i].patches[y].patch.size(); z++)
								{
									temp_patch[z] = hacks_hash[i].patches[y].patch[z];
									//gprintf("patch[y][z] = 0x%08X\ntemp_patch=0x%08X\n",hacks_hash[i].patch[y][z],*temp_patch);
								}
								memcpy(mem_block+add,temp_patch,sizeof(temp_patch) );
								DCFlushRange((u8 *)((add+(u32)mem_block) >> 5 << 5), (sizeof(temp_patch) >> 5 << 5) + 64);
								//gprintf("patch : 0x%08X\npatched address : 0x%08X\n",temp_patch, *(vu32*)( add+ (u32)mem_block));
								break;
							}
							add++;
						}//end while loop
					} //end for loop of all hashes of hack i
				} //end if state = 1
			} // end general hacks loop
		} //end if hacks > 0
	} // end if classic hack are nto enabled
	//exit(0);
#ifdef DEBUG
	sleep(20);
#endif
	ShutdownDevices();
	USB_Deinitialize();
	if(init == 1 || SGetSetting(SETTING_SHOWGECKOTEXT) != 0 )
		Control_VI_Regs(2);
	__STM_Close();
	ISFS_Deinitialize();
	__IOS_ShutdownSubsystems();
	mtmsr(mfmsr() & ~0x8000);
	mtmsr(mfmsr() | 0x2002);
	ICSync();
	_unstub_start();
free_and_return:
	if(rTMD)
	{
		free_pointer(rTMD);
	}
	if(TMD)
	{
		free_pointer(TMD);
	}
	if(tstatus)
	{
		free_pointer( tstatus );
	}
	if(buf)
	{
		free_pointer( buf );
	}
	if(boot_hdr)
	{
		free_pointer(boot_hdr);
	}
	return;
}
void InstallLoadDOL( void )
{
	char filename[MAXPATHLEN],filepath[MAXPATHLEN];
	std::vector<char*> names;
	
	struct stat st;
	DIR_ITER* dir;

	if(Mounted == 0)
	{
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("NO fat device found!"))*13/2))>>1, 208, "NO fat device found!");
		sleep(5);
		return;
	}
	s8 DevStat = Mounted;
	dir = diropen ("fat:/");
	if( dir == NULL )
	{
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Failed to open root of Device!"))*13/2))>>1, 208, "Failed to open root of Device!");
		sleep(5);
		return;
	}
	//get all files names
	while( dirnext (dir, filename, &st) != -1 )
	{
		if( (strstr( filename, ".dol") != NULL) ||
			(strstr( filename, ".DOL") != NULL) ||
			(strstr( filename, ".elf") != NULL) ||
			(strstr( filename, ".ELF") != NULL) )
		{
			names.resize( names.size() + 1 );
			names[names.size()-1] = new char[strlen(filename)+1];
			memcpy( names[names.size()-1], filename, strlen( filename ) + 1 );
		}
	}

	dirclose( dir );

	if( names.size() == 0 )
	{
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Couldn't find any executable files"))*13/2))>>1, 208, "Couldn't find any executable files");
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("in the root of the FAT device!"))*13/2))>>1, 228, "in the root of the FAT device!");
		sleep(5);
		return;
	}

	u32 redraw = 1;
	u32 cur_off= 0;

	while(1)
	{
		WPAD_ScanPads();
		PAD_ScanPads();

		u32 WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
		u32 PAD_Pressed  = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);
 
#ifdef DEBUG
		if ( (WPAD_Pressed & WPAD_BUTTON_HOME) || (PAD_Pressed & PAD_BUTTON_START) )
			exit(0);
#endif
		if ( WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B )
		{
			break;
		}

		if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
		{
			ClearScreen();
			if ( ( (Mounted & 2) && !__io_wiisd.isInserted() ) || ( (Mounted & 1) && !__io_usbstorage.isInserted() ) )
			{
				gprintf("SD/USB not in same state\n");
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("FAT device removed before loading!"))*13/2))>>1, 208, "FAT device removed before loading!");
				sleep(5);
				break;
			}
			//Install file
			sprintf(filepath, "fat:/%s",names[cur_off]);
			FILE *dol = fopen(filepath, "rb" );
			if( dol == NULL )
			{
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Could not open:\"%s\" for reading")+strlen(names[cur_off]))*13/2))>>1, 208, "Could not open:\"%s\" for reading", names[cur_off]);
				sleep(5);
				break;
			}
			PrintFormat( 0, ((rmode->viWidth /2)-((strlen("Installing \"%s\"...")+strlen(names[cur_off]))*13/2))>>1, 208, "Installing \"%s\"...", names[cur_off]);

			//get size
			fseek( dol, 0, SEEK_END );
			unsigned int size = ftell( dol );
			fseek( dol, 0, 0 );

			char *buf = (char*)memalign( 32, sizeof( char ) * size );
			memset( buf, 0, sizeof( char ) * size );

			fread( buf, sizeof( char ), size, dol );
			fclose( dol );

			//Check if there is already a main.dol installed
			s32 fd = ISFS_Open("/title/00000001/00000002/data/main.bin", 1|2 );

			if( fd >= 0 )	//delete old file
			{
				ISFS_Close( fd );
				ISFS_Delete("/title/00000001/00000002/data/main.bin");
			}

			//file not found create a new one
			ISFS_CreateFile("/title/00000001/00000002/data/main.bin", 0, 3, 3, 3);
			fd = ISFS_Open("/title/00000001/00000002/data/main.bin", 1|2 );

			if( ISFS_Write( fd, buf, sizeof( char ) * size ) != (signed)(sizeof( char ) * size) )
			{
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Writing file failed!"))*13/2))>>1, 240, "Writing file failed!");
			} else {
				PrintFormat( 0, ((rmode->viWidth /2)-((strlen("\"%s\" installed")+strlen(names[cur_off]))*13/2))>>1, 240, "\"%s\" installed", names[cur_off]);
			}

			sleep(5);
			ClearScreen();
			redraw=true;
			ISFS_Close( fd );
			free_pointer( buf );

		}

		if ( WPAD_Pressed & WPAD_BUTTON_2 || WPAD_Pressed & WPAD_CLASSIC_BUTTON_X || PAD_Pressed & PAD_BUTTON_X )
		{
			ClearScreen();
			//Delete file

			PrintFormat( 0, ((rmode->viWidth /2)-((strlen("Deleting installed File..."))*13/2))>>1, 208, "Deleting installed File...");

			//Check if there is already a main.dol installed
			s32 fd = ISFS_Open("/title/00000001/00000002/data/main.bin", 1|2 );

			if( fd >= 0 )	//delete old file
			{
				ISFS_Close( fd );
				ISFS_Delete("/title/00000001/00000002/data/main.bin");

				fd = ISFS_Open("/title/00000001/00000002/data/main.bin", 1|2 );

				if( fd >= 0 )	//file not delete
					PrintFormat( 0, ((rmode->viWidth /2)-((strlen("Failed"))*13/2))>>1, 240, "Failed");
				else
					PrintFormat( 0, ((rmode->viWidth /2)-((strlen("Success"))*13/2))>>1, 240, "Success");
			}
			else
				PrintFormat( 0, ((rmode->viWidth /2)-((strlen("No File installed..."))*13/2))>>1, 240, "No File installed...");

			sleep(5);
			ClearScreen();
			redraw=true;
			ISFS_Close( fd );

		}

		if ( WPAD_Pressed & WPAD_BUTTON_1 || WPAD_Pressed & WPAD_CLASSIC_BUTTON_Y || PAD_Pressed & PAD_BUTTON_Y )
		{
			ClearScreen();
			if ( ( (Mounted & 2) && !__io_wiisd.isInserted() ) || ( (Mounted & 1) && !__io_usbstorage.isInserted() ) )
			{
				gprintf("SD/USB not in same state\n");
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("FAT device removed before loading!"))*13/2))>>1, 208, "FAT device removed before loading!");
				sleep(5);
				break;
			}
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Loading binary..."))*13/2))>>1, 208, "Loading binary...");
			//Load binary
			sprintf(filepath, "fat:/%s", names[cur_off]);
			BootDolFromDir(filepath);
			sleep(1);
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("failed to load binary"))*13/2))>>1, 224, "failed to load binary");
			sleep(3);
			ClearScreen();
			redraw=true;
		}

		if ( WPAD_Pressed & WPAD_BUTTON_DOWN || WPAD_Pressed & WPAD_CLASSIC_BUTTON_DOWN || PAD_Pressed & PAD_BUTTON_DOWN )
		{
			cur_off++;

			if( cur_off >= names.size())
				cur_off = 0;
			
			redraw=true;
		} else if ( WPAD_Pressed & WPAD_BUTTON_UP || WPAD_Pressed & WPAD_CLASSIC_BUTTON_UP || PAD_Pressed & PAD_BUTTON_UP )
		{
			if ( cur_off != 0)
				cur_off--;
			else if ( cur_off == 0)
				cur_off=names.size()-1;
			
			redraw=true;
		}

		if( redraw )
		{
			for( u32 i=0; i<names.size(); ++i )
				PrintFormat( cur_off==i, 16, 64+i*16, "%s", names[i]);

			PrintFormat( 0, ((rmode->viWidth /2)-((strlen("A(A) Install File"))*13/2))>>1, rmode->viHeight-64, "A(A) Install FIle");
			PrintFormat( 0, ((rmode->viWidth /2)-((strlen("1(Z) Load File   "))*13/2))>>1, rmode->viHeight-48, "1(Y) Load File");
			PrintFormat( 0, ((rmode->viWidth /2)-((strlen("2(X) Delete installed File"))*13/2))>>1, rmode->viHeight-32, "2(X) Delete installed File");

			redraw = false;
		}
		PollDevices();
		if (DevStat != Mounted)
		{
			gprintf("DevStat = %d%d & Mounted = %d%d\n",(DevStat&2),(DevStat&1),(Mounted&2),(Mounted&1));
			ClearScreen();
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Reloading Binaries..."))*13/2))>>1, 208, "Reloading Binaries...");
			sleep(2);
			for( u32 i=0; i<names.size(); ++i )
				delete names[i];
			names.clear();
			if(Mounted == 0)
			{
				ClearScreen();
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("NO fat device found!"))*13/2))>>1, 208, "NO fat device found!");
				sleep(5);
				return;
			}
			dir = diropen ("fat:/");
			if( dir == NULL )
			{
				ClearScreen();
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Failed to open root of Device!"))*13/2))>>1, 208, "Failed to open root of Device!");
				sleep(5);
				return;
			}
			//get all files names
			while( dirnext (dir, filename, &st) != -1 )
			{
				if( (strstr( filename, ".dol") != NULL) ||
					(strstr( filename, ".DOL") != NULL) ||
					(strstr( filename, ".elf") != NULL) ||
					(strstr( filename, ".ELF") != NULL) )
				{
					names.resize( names.size() + 1 );
					names[names.size()-1] = new char[strlen(filename)+1];
					memcpy( names[names.size()-1], filename, strlen( filename ) + 1 );
				}
			}

			dirclose( dir );

			if( names.size() == 0 )
			{
				ClearScreen();
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Couldn't find any executable files"))*13/2))>>1, 208, "Couldn't find any executable files");
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("in the root of the FAT device!"))*13/2))>>1, 228, "in the root of the FAT device!");
				sleep(5);
				return;
			}
			ClearScreen();
			redraw = 1;
			DevStat = Mounted;
		}
		VIDEO_WaitVSync();
	}

	//free memory
	for( u32 i=0; i<names.size(); ++i )
		delete names[i];
	names.clear();

	return;
}
void AutoBootDol( void )
{
	s32 fd = ISFS_Open("/title/00000001/00000002/data/main.bin", 1 );
	if( fd < 0 )
	{
		error = ERROR_BOOT_DOL_OPEN;
		return;
	}

	void	(*entrypoint)();

	Elf32_Ehdr *ElfHdr = (Elf32_Ehdr *)memalign( 32, (sizeof( Elf32_Ehdr )+31)&(~31) );
	if( ElfHdr == NULL )
	{
		error = ERROR_MALLOC;
		return;
	}

	s32 r = ISFS_Read( fd, ElfHdr, sizeof( Elf32_Ehdr ) );
	if( r < 0 || r != sizeof( Elf32_Ehdr ) )
	{
		error = ERROR_BOOT_DOL_READ;
		return;
	}

	if( ElfHdr->e_ident[EI_MAG0] == 0x7F ||
		ElfHdr->e_ident[EI_MAG1] == 'E' ||
		ElfHdr->e_ident[EI_MAG2] == 'L' ||
		ElfHdr->e_ident[EI_MAG3] == 'F' )
	{
#ifdef DEBUG
		gprintf("ELF Found\n");
		gprintf("Type:      \t%04X\n", ElfHdr->e_type );
		gprintf("Machine:   \t%04X\n", ElfHdr->e_machine );
		gprintf("Version:  %08X\n", ElfHdr->e_version );
		gprintf("Entry:    %08X\n", ElfHdr->e_entry );
		gprintf("Flags:    %08X\n", ElfHdr->e_flags );
		gprintf("EHsize:    \t%04X\n\n", ElfHdr->e_ehsize );

		gprintf("PHoff:    %08X\n",	ElfHdr->e_phoff );
		gprintf("PHentsize: \t%04X\n",	ElfHdr->e_phentsize );
		gprintf("PHnum:     \t%04X\n\n",ElfHdr->e_phnum );

		gprintf("SHoff:    %08X\n",	ElfHdr->e_shoff );
		gprintf("SHentsize: \t%04X\n",	ElfHdr->e_shentsize );
		gprintf("SHnum:     \t%04X\n",	ElfHdr->e_shnum );
		gprintf("SHstrndx:  \t%04X\n\n",ElfHdr->e_shstrndx );
#endif

		if( ElfHdr->e_phnum == 0 )
		{
#ifdef DEBUG
			printf("Warning program header entries are zero!\n");
#endif
		} else {

			for( int i=0; i < ElfHdr->e_phnum; ++i )
			{
				r = ISFS_Seek( fd, ElfHdr->e_phoff + sizeof( Elf32_Phdr ) * i, SEEK_SET );
				if( r < 0 )
				{
					error = ERROR_BOOT_DOL_SEEK;
					free_pointer(ElfHdr);
					return;
				}

				Elf32_Phdr *phdr = (Elf32_Phdr *)memalign( 32, (sizeof( Elf32_Phdr )+31)&(~31) );
				if( phdr == NULL )
				{
					error = ERROR_MALLOC;
					free_pointer(ElfHdr);
					return;
				}
				memset(phdr,0,sizeof(Elf32_Phdr));
				r = ISFS_Read( fd, phdr, sizeof( Elf32_Phdr ) );
				if( r < 0 )
				{
					error = ERROR_BOOT_DOL_READ;
					free_pointer( phdr );
					free_pointer(ElfHdr);
					return;
				}
#ifdef DEBUG
				gprintf("Type:%08X Offset:%08X VAdr:%08X PAdr:%08X FileSz:%08X\n", phdr->p_type, phdr->p_offset, phdr->p_vaddr, phdr->p_paddr, phdr->p_filesz );
#endif
				r = ISFS_Seek( fd, phdr->p_offset, 0 );
				if( r < 0 )
				{
					error = ERROR_BOOT_DOL_SEEK;
					free_pointer( phdr );
					free_pointer(ElfHdr);
					return;
				}

				if(phdr->p_type == PT_LOAD && phdr->p_vaddr != 0)
				{
					r = ISFS_Read( fd, (void*)phdr->p_vaddr, phdr->p_filesz);
					if( r < 0 )
					{
						gprintf("AutoBootDol : ISFS_Read(%d)read failed of the program section addr\n",r);
						error = ERROR_BOOT_DOL_READ;
						free_pointer( phdr );
						free_pointer(ElfHdr);
						return;
					}
				}
				free_pointer( phdr );
			}
		}
		if( ElfHdr->e_shnum == 0 )
		{
#ifdef DEBUG
			printf("Warning section header entries are zero!\n");
#endif
		} else {

			Elf32_Shdr *shdr = (Elf32_Shdr *)memalign( 32, (sizeof( Elf32_Shdr )+31)&(~31) );
			if( shdr == NULL )
			{
				error = ERROR_MALLOC;
				free_pointer(ElfHdr);
				return;
			}
			memset(shdr,0,sizeof(Elf32_Shdr));
			for( int i=0; i < ElfHdr->e_shnum; ++i )
			{
				r = ISFS_Seek( fd, ElfHdr->e_shoff + sizeof( Elf32_Shdr ) * i, SEEK_SET );
				if( r < 0 )
				{
					error = ERROR_BOOT_DOL_SEEK;
					free_pointer( shdr );
					free_pointer(ElfHdr);
					return;
				}

				r = ISFS_Read( fd, shdr, sizeof( Elf32_Shdr ) );
				if( r < 0 )
				{
					error = ERROR_BOOT_DOL_READ;
					free_pointer( shdr );
					free_pointer(ElfHdr);
					return;
				}

				if( shdr->sh_type == 0 || shdr->sh_size == 0 )
					continue;

#ifdef DEBUG
				if( shdr->sh_type > 17 )
					printf("Warning the type: %08X could be invalid!\n", shdr->sh_type );

				if( shdr->sh_flags & ~0xF0000007 )
					printf("Warning the flag: %08X is invalid!\n", shdr->sh_flags );

				printf("Type:%08X Offset:%08X Name:%08X Off:%08X Size:%08X\n", shdr->sh_type, shdr->sh_offset, shdr->sh_name, shdr->sh_addr, shdr->sh_size );
#endif
				r = ISFS_Seek( fd, shdr->sh_offset, 0 );
				if( r < 0 )
				{
					error = ERROR_BOOT_DOL_SEEK;
					free_pointer( shdr );
					free_pointer(ElfHdr);
					return;
				}

				
				if (shdr->sh_type == SHT_NOBITS)
				{
					r = ISFS_Read( fd, (void*)(shdr->sh_addr | 0x80000000), shdr->sh_size);
					if( r < 0 )
					{
						gprintf("AutoBootDol : ISFS_Read(%d) data header\n",r);
						error = ERROR_BOOT_DOL_READ;
						free_pointer( shdr );
						free_pointer(ElfHdr);
						return;
					}
				}

			}
			free_pointer( shdr );
		}

		ISFS_Close( fd );
		entrypoint = (void (*)())(ElfHdr->e_entry | 0x80000000);
		free_pointer(ElfHdr);

	} else {
		//Dol
		//read the header
		dolhdr *hdr = (dolhdr *)memalign(32, (sizeof( dolhdr )+31)&(~31) );
		if( hdr == NULL )
		{
			error = ERROR_MALLOC;
			return;
		}

		s32 r = ISFS_Seek( fd, 0, 0);
		if( r < 0 )
		{
			gprintf("AutoBootDol : ISFS_Seek(%d)\n", r);
			error = ERROR_BOOT_DOL_SEEK;
			return;
		}

		r = ISFS_Read( fd, hdr, sizeof(dolhdr) );

		if( r < 0 || r != sizeof(dolhdr) )
		{
			gprintf("AutoBootDol : ISFS_Read(%d)\n", r);
			error = ERROR_BOOT_DOL_READ;
			return;
		}
#ifdef DEBUG
		gprintf("\nText Sections:\n");
#endif

		int i=0;
		for (i = 0; i < 6; i++)
		{
			if( hdr->sizeText[i] && hdr->addressText[i] && hdr->offsetText[i] )
			{
				if(ISFS_Seek( fd, hdr->offsetText[i], SEEK_SET )<0)
				{
					error = ERROR_BOOT_DOL_SEEK;
					return;
				}
				if(ISFS_Read( fd, (void*)(hdr->addressText[i]), hdr->sizeText[i] )<0)
				{
					error = ERROR_BOOT_DOL_READ;
					return;
				}
				DCInvalidateRange( (void*)(hdr->addressText[i]), hdr->sizeText[i] );
#ifdef DEBUG
				gprintf("\t%08x\t\t%08x\t\t%08x\t\t\n", (hdr->offsetText[i]), hdr->addressText[i], hdr->sizeText[i]);
#endif
			}
		}
#ifdef DEBUG
		gprintf("\nData Sections:\n");
#endif
		// data sections
		for (i = 0; i <= 10; i++)
		{
			if( hdr->sizeData[i] && hdr->addressData[i] && hdr->offsetData[i] )
			{
				if(ISFS_Seek( fd, hdr->offsetData[i], SEEK_SET )<0)
				{
					error = ERROR_BOOT_DOL_SEEK;
					return;
				}
				if( ISFS_Read( fd, (void*)(hdr->addressData[i]), hdr->sizeData[i] )<0)
				{
					error = ERROR_BOOT_DOL_READ;
					return;
				}

				DCInvalidateRange( (void*)(hdr->addressData[i]), hdr->sizeData[i] );
#ifdef DEBUG
				gprintf("\t%08x\t\t%08x\t\t%08x\t\t\n", (hdr->offsetData[i]), hdr->addressData[i], hdr->sizeData[i]);
#endif
			}
		}

		entrypoint = (void (*)())(hdr->entrypoint);

	}
	if( entrypoint == 0x00000000 )
	{
		error = ERROR_BOOT_DOL_ENTRYPOINT;
		return;
	}
	DVDStopDisc(false);
	for(int i=0;i<WPAD_MAX_WIIMOTES;i++) {
		WPAD_Flush(i);
		WPAD_Disconnect(i);
	}
	WPAD_Shutdown();
	if( ClearState() < 0 )
	{
		gprintf("ClearState failure\n");
	}
	ISFS_Deinitialize();
	ShutdownDevices();
	USB_Deinitialize();
	gprintf("Entrypoint: %08X\n", (u32)(entrypoint) );

	if(ReloadedIOS)
	{
		if( isIOSstub(58) )
		{
			if( isIOSstub(IOS_GetPreferredVersion()) )
			{
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("failed to reload ios for homebrew! ios is a stub!"))*13/2))>>1, 208, "failed to reload ios for homebrew! ios is a stub!");
				sleep(3);
			}
			else
			{
				IOS_ReloadIOS(IOS_GetPreferredVersion());
				ReloadedIOS = 1;
			}
		}
		else
		{
			IOS_ReloadIOS(58);
			ReloadedIOS = 1;
		}
	}

	__IOS_ShutdownSubsystems();
	//slightly modified loading code from USBLOADER GX...
	u32 level;
	SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
	_CPU_ISR_Disable (level);
	mtmsr(mfmsr() & ~0x8000);
	mtmsr(mfmsr() | 0x2002);
	entrypoint();
	_CPU_ISR_Restore (level);
	//never gonna happen; but failsafe
	WPAD_Init();
	PAD_Init();
	ISFS_Initialize();
	return;
}
void HandleWiiMoteEvent(s32 chan)
{
	Shutdown=1;
}
s8 CheckTitleOnSD(u64 id)
{
	//check Mounted Devices so that IF the SD is inserted it also gets mounted
	PollDevices();
	if (!__io_wiisd.isInserted()) //no SD inserted, lets bail out
		return -1;
	char title_ID[5];
	//Check app on SD. it might be there. not that it matters cause we can't boot from SD
	memset(title_ID,0,5);
	u32 title_l = id & 0xFFFFFFFF;
	memcpy(title_ID, &title_l, 4);
	for (s8 f=0; f<4; f++)
	{
		if(title_ID[f] < 0x20)
			title_ID[f] = '.';
		if(title_ID[f] > 0x7E)
			title_ID[f] = '.';
	}
	title_ID[4]='\0';
	char file[256] ATTRIBUTE_ALIGN(32);
	memset(file,0,256);
	sprintf(file, "fat:/private/wii/title/%s/content.bin", title_ID);
#ifdef DEBUG
	gprintf ("CheckTitleOnSD : %s\n",file);
#endif
	FILE* SDHandler = fopen(file,"rb");
	if (SDHandler)
	{
		//content.bin is there meaning its on SD
		fclose(SDHandler);
		gprintf("CheckTitleOnSD : title is saved on SD\n");
		return 1;
	}
	else
	{
		//title isn't on SD either. ow well...
		gprintf("CheckTitleOnSD : content not found on NAND or SD\n");
		return 0;
	}
}
s8 GetTitleName(u64 id, u32 app, char* name) {
	s32 r;
    int lang = 1; //CONF_GetLanguage();
    /*
    languages:
    enum {
	CONF_LANG_JAPANESE = 0,
	CONF_LANG_ENGLISH,
	CONF_LANG_GERMAN,
	CONF_LANG_FRENCH,
	CONF_LANG_SPANISH,
	CONF_LANG_ITALIAN,
	CONF_LANG_DUTCH,
	CONF_LANG_SIMP_CHINESE,
	CONF_LANG_TRAD_CHINESE,
	CONF_LANG_KOREAN
	};
	cause we dont support unicode stuff in font.cpp we will force to use english then(1)
    */
    char file[256] ATTRIBUTE_ALIGN(32);
	memset(file,0,256);
    sprintf(file, "/title/%08x/%08x/content/%08x.app", (u32)(id >> 32), (u32)id, app);
#ifdef DEBUG
	gprintf("GetTitleName : %s\n",file);
#endif
	u32 cnt ATTRIBUTE_ALIGN(32);
	cnt = 0;
	IMET *data = (IMET *)memalign(32, (sizeof(IMET)+31)&(~31));
	if(data == NULL)
	{
		gprintf("GetTitleName : IMET header align failure\n");
		return -1;
	}
	memset(data,0,(sizeof(IMET)+31)&(~31));
	r = ES_GetNumTicketViews(id, &cnt);
	if(r < 0)
	{
		gprintf("GetTitleName : GetNumTicketViews error %d!\n",r);
		free_pointer(data);
		return -1;
	}
	tikview *views = (tikview *)memalign( 32, sizeof(tikview)*cnt );
	if(views == NULL)
	{
		free_pointer(data);
		return -2;
	}
	r = ES_GetTicketViews(id, views, cnt);
	if (r < 0)
	{
		gprintf("GetTitleName : GetTicketViews error %d \n",r);
		free_pointer(data);
		free_pointer(views);
		return -2;
	}

	//lets get this party started with the right way to call ES_OpenTitleContent. and not like how libogc < 1.8.3 does it. patch was passed on , and is done correctly in 1.8.3
	//the right way is ES_OpenTitleContent(u64 TitleID,tikview* views,u16 Index); note the views >_>
	s32 fh = ES_OpenTitleContent(id, views, 0);
	if (fh == -106)
	{
		gprintf("GetTitleName : app not found\n",fh);
		CheckTitleOnSD(id);
		free_pointer(data);
		free_pointer(views);
		return -3;
	}
	else if(fh < 0)
	{
		//ES method failed. remove tikviews from memory and fall back on ISFS method
		gprintf("GetTitleName : ES_OpenTitleContent error %d\nGetTitleName : ISFS : ",fh);
		free_pointer(views);
		fh = ISFS_Open(file, ISFS_OPEN_READ);
		// fuck failed. lets check SD & GTFO
		if (fh == -106)
		{
			CheckTitleOnSD(id);
			return -4;
		}
		else if (fh < 0)
		{
			free_pointer(data);
			gprintf("open %s error %d\n",file,fh);
			return -5;
		}
		// read the completed IMET header
		r = ISFS_Read(fh, data, sizeof(IMET));
		if (r < 0) {
			gprintf("IMET read error %d\n",r);
			ISFS_Close(fh);
			free_pointer(data);
			return -6;
		}
		gprintf("success\n");
		ISFS_Close(fh);
	}
	else
	{
		//ES method
		u8* IMET_data = (u8*)memalign(32, (sizeof(IMET)+31)&(~31));
		if(IMET_data == NULL)
		{
			gprintf("GetTitleName : IMET header align failure\n");
			return -7;
		}
		r = ES_ReadContent(fh,IMET_data,sizeof(IMET));
		if (r < 0) {
			gprintf("GetTitleName : ES_ReadContent error %d\n",r);
			ES_CloseContent(fh);
			free_pointer(IMET_data);
			free_pointer(data);
			free_pointer(views);
			return -8;
		}
		//free data and let it point to IMET_data so everything else can work just fine
		free_pointer(data);
		data = (IMET*)IMET_data;
		ES_CloseContent(fh);
		free_pointer(views);
	}
	char str[10][84];
	//clear any memory that is in the place of the array cause we dont want any confusion here
	memset(str,0,10*84);
	for(u8 y =0;y <= 9;y++)
	{
		u8 j;
		u8 r = 0;
		for(j=0;j<83;j++)
		{
			if(data->names[y][j] < 0x20)
				continue;
			else if(data->names[y][j] > 0x7E)
				continue;
			else
			{
				str[y][r++] = data->names[y][j];
			}
		}
		str[y][83] = '\0';

	}
	free_pointer(data);
	if(str[lang][0] != '\0')
	{
#ifdef DEBUG
		gprintf("GetTitleName : title %s\n",str[lang]);
#endif
		sprintf(name, "%s", str[lang]);
	}
	else
		gprintf("GetTitleName: no name found\n");
	memset(str,0,10*84);
	return 1;
}
s32 LoadListTitles( void )
{
	s32 ret;
	u32 count = 0;
	ret = ES_GetNumTitles(&count);
	if (ret < 0)
	{
		gprintf("GetNumTitles error %d\n",ret);
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Failed to get the amount of installed titles!"))*13/2))>>1, 208+16, "Failed to get the amount of installed titles!");
		sleep(3);
		return ret;
	}
	gprintf("%u titles\n",count);
	static u64 title_list[256] ATTRIBUTE_ALIGN(32);
	ret = ES_GetTitles(title_list, count);
	if (ret < 0) {
		gprintf("ES_GetTitles error %d\n",ret);
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Failed to get the titles list!"))*13/2))>>1, 208+16, "Failed to get the titles list!");
		sleep(3);
		return ret;
	}
	if(count == 0)
	{
		gprintf("count == 0\n",ret);
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Failed to get the titles list!"))*13/2))>>1, 208+16, "Failed to get the titles list!");
		sleep(3);
		return ret;
	}
	std::vector<u64> list;
	std::vector<std::string> titles_ascii;
	tmd_view *rTMD;
	char temp_name[256];
	char title_ID[5];
	list.clear();
	titles_ascii.clear();
	for(u32 i = 0;i < count;i++)
	{	
		//u32 titletype = title_list[i] >> 32;
		switch (title_list[i] >> 32)
		{
			case 1: // IOS, MIOS, BC, System Menu
			case 0x10000: // TMD installed by running a disc
			case 0x10004: // "Hidden channels by discs" -- WiiFit channel
			case 0x10008: // "Hidden channels" -- EULA, rgnsel
			case 0x10005: // Downloadable Content for Wiiware
			default:
				break;
			case 0x10001: // Normal channels / VC
			case 0x10002: // "System channels" -- News, Weather, etc.
				u32 tmd_size;
				ret = ES_GetTMDViewSize(title_list[i], &tmd_size);
				if(ret<0)
				{
					gprintf("WARNING : GetTMDViewSize error %d on title %x-%x\n",ret,title_list[i],(u32)title_list[i]);
					PrintFormat( 1, ((rmode->viWidth /2)-((strlen("WARNING : TMDSize error on 00000000-00000000!"))*13/2))>>1, 208+16, "WARNING : TMDSize error on %08X-%08X",title_list[i],(u32)title_list[i]);
					sleep(3);
					ClearScreen();
					continue;
				}
				rTMD = (tmd_view*)memalign( 32, (tmd_size+31)&(~31) );
				if( rTMD == NULL )
				{
					PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Failed to MemAlign TMD!"))*13/2))>>1, 208+16, "Failed to MemAlign TMD!");
					sleep(3);
					return 0;
				}
				memset(rTMD,0, (tmd_size+31)&(~31) );
				ret = ES_GetTMDView(title_list[i], (u8*)rTMD, tmd_size);
				if(ret<0)
				{
					gprintf("WARNING : GetTMDView error %d on title %x-%x\n",ret,title_list[i],(u32)title_list[i]);
					PrintFormat( 1, ((rmode->viWidth /2)-((strlen("WARNING : TMD error on 00000000-00000000!"))*13/2))>>1, 208+16, "WARNING : TMD error on %08X-%08X!",title_list[i],(u32)title_list[i]);
					sleep(3);
					if(rTMD)
					{
						free_pointer(rTMD);
					}
					ClearScreen();
					continue;
				}
				sprintf(temp_name,"????????");
				ret = GetTitleName(rTMD->title_id,rTMD->contents[0].cid,temp_name);
				if ( ret != -3 && ret != -4 )
				{
#ifdef DEBUG
					gprintf("LoadListTitles : added %s\n",temp_name);
#endif
					list.push_back(title_list[i]);
					titles_ascii.push_back(temp_name);
				}
				if ( ret < 0 )
				{
#ifdef _DEBUG
					gprintf("title %x-%x is either on SD/deleted or IOS trouble came up\n",title_list[i],(u32)title_list[i]);
#endif
					gprintf("LoadListTitles : GetTitleName error %d\n",ret);
				}
				if(rTMD)
				{
					free_pointer(rTMD);
				}
				break;
		}
	}
	//done detecting titles. lets list them
	if(list.size() <= 0)
	{
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("ERROR : No VC/Wiiware channels found"))*13/2))>>1, 208+16, "ERROR : No VC/Wiiware channels found");
		sleep(3);
		return 0;
	}
	s8 redraw = true;
	s16 cur_off = 0;
	//eventho normally a tv would be able to show 23 titles; some TV's do 60hz in a horrible mannor 
	//making title 23 out of the screen just like the main menu
	s16 max_pos;
	if( rmode->viTVMode == VI_NTSC || CONF_GetEuRGB60() || CONF_GetProgressiveScan() )
	{
		//ye, those tv's want a special treatment again >_>
		max_pos = 18;
	}
	else
	{
		max_pos = 23;
	}
	s16 min_pos = 0;
	if ((s32)list.size() < max_pos)
		max_pos = list.size() -1;
	while(1)
	{
		WPAD_ScanPads();
		PAD_ScanPads();

		u32 WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
		u32 PAD_Pressed  = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);
		if ( WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B )
		{
			if(titles_ascii.size())
				titles_ascii.clear();
			if(list.size())
				list.clear();
			break;
		}
		if ( WPAD_Pressed & WPAD_BUTTON_UP || WPAD_Pressed & WPAD_CLASSIC_BUTTON_UP || PAD_Pressed & PAD_BUTTON_UP )
		{
			cur_off--;
			if (cur_off < min_pos)
			{
				min_pos = cur_off;
				if(list.size() > 23)
					ClearScreen();
			}
			if (cur_off < 0)
			{
				cur_off = list.size() - 1;
				min_pos = list.size() - max_pos - 1;
			}
			redraw = true;
		}
		if ( WPAD_Pressed & WPAD_BUTTON_DOWN || WPAD_Pressed & WPAD_CLASSIC_BUTTON_DOWN || PAD_Pressed & PAD_BUTTON_DOWN )
		{
			cur_off++;
			if (cur_off > (max_pos + min_pos))
			{
				min_pos = cur_off - max_pos;
				if(list.size() > 23)
				{
					ClearScreen();
				}
			}
			if (cur_off >= (s32)list.size())
			{
				cur_off = 0;
				min_pos = 0;
			}
			redraw = true;
		}
		if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
		{
			DVDStopDisc(true);
			ClearScreen();
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Loading title..."))*13/2))>>1, 208, "Loading title...");

			//lets start this bitch
			u32 cnt ATTRIBUTE_ALIGN(32) = 0;
			tikview *views = 0;
			if (ES_GetNumTicketViews(list[cur_off], &cnt) < 0)
			{
				gprintf("GetNumTicketViews failure\n");
				break;
			}
			views = (tikview *)memalign( 32, sizeof(tikview)*cnt );
			if(views == NULL)
			{
				gprintf("memalign views failure\n");
				break;
			}
			memset(views,0,sizeof(tikview));
			if (ES_GetTicketViews(list[cur_off], views, cnt) < 0 )
			{
				gprintf("ES_GetTicketViews failure!\n");
				break;
			}
			if( ClearState() < 0 )
			{
				gprintf("ClearState failure\n");
			}
			VIDEO_ClearFrameBuffer( rmode, xfb, COLOR_BLACK);
			VIDEO_WaitVSync();
			ES_LaunchTitle(list[cur_off], &views[0]);
			PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Failed to Load Title!"))*13/2))>>1, 224, "Failed to Load Title!");
			sleep(3);
			free_pointer(views);
			redraw = true;
		}			
		if(redraw)
		{
			s8 i= min_pos;
			if((s32)list.size() > max_pos && (min_pos != (s32)list.size() - max_pos - 1))
			{
				PrintFormat( 0,((rmode->viWidth /2)-((strlen("-----More-----"))*13/2))>>1,64+(max_pos+2)*16,"-----More-----");
			}
			if(min_pos > 0)
			{
				PrintFormat( 0,((rmode->viWidth /2)-((strlen("-----Less-----"))*13/2))>>1,64,"-----Less-----");
			}
			for(; i<=(min_pos + max_pos); i++ )
			{
				memset(title_ID,0,5);
				u32 title_l = list[i] & 0xFFFFFFFF;
				memcpy(title_ID, &title_l, 4);
				for (s8 f=0; f<4; f++)
				{
					if(title_ID[f] < 0x20)
						title_ID[f] = '.';
					if(title_ID[f] > 0x7E)
						title_ID[f] = '.';
				}
				title_ID[4]='\0';
				PrintFormat( cur_off==i, 16, 64+(i-min_pos+1)*16, "(%d)%s(%s)                   ",i+1,titles_ascii[i].c_str(), title_ID);
				PrintFormat( 0, ((rmode->viWidth /2)-((strlen("A(A) Load Title       "))*13/2))>>1, rmode->viHeight-32, "A(A) Load Title");
			}
			redraw = false;
		}
		VIDEO_WaitVSync();
	}
	return 0;
}
void CheckForUpdate()
{
	ClearScreen();
	PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Initialising Wifi..."))*13/2))>>1, 208, "Initialising Wifi...");
	if (InitNetwork() < 0 )
	{
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("failed to initialise wifi"))*13/2))>>1, 224, "failed to initialise wifi");
		sleep(5);
		return;
	}
	UpdateStruct *UpdateFile = NULL;
	s32 file_size = GetHTTPFile("www.dacotaco.com","/priiloader/version.dat",(u8*&)UpdateFile,0);
	if ( file_size <= 0 || file_size != (s32)sizeof(UpdateStruct))
	{
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("error getting versions from server"))*13/2))>>1, 224, "error getting versions from server");
		if (file_size < -9)
		{
			//free pointer
			free_pointer(UpdateFile);
		}
		if (file_size < 0 && file_size > -4)
		{
			//errors connecting to server
			gprintf("connection failure. error %d\n",file_size);
		}
		else if (file_size == -7)
		{
			gprintf("CheckForUpdate : HTTP Error %s!\n",Get_Last_reply());
		}
		else if ( file_size < 0 )
		{
			gprintf("CheckForUpdate : GetHTTPFile error %d\n",file_size);
		}
		else if (file_size != (s32)sizeof(UpdateStruct))
		{
			gprintf("CheckForUpdate : file_size != UpdateStruct\n");
		}
		sleep(5);
		free_pointer(UpdateFile);
		net_deinit();
		return;
	}


//generate update list if any
//----------------------------------------
	file_size = 0;
	u8* Data = NULL;
	u8 DownloadedBeta = 0;
	u8 BetaUpdates = 0;
	u8 VersionUpdates = 0;
	u8* Changelog = NULL;
	u8 redraw = 1;
	s8 cur_off = 0;
	s8 Language = 0;
	UpdateStruct* UpdateGer = NULL;
	UpdateStruct* UpdateFr = NULL;
	UpdateStruct* UpdateSp = NULL;
	ClearScreen();
	//make a nice list of the updates
	if ( (VERSION < UpdateFile->version) || (VERSION == UpdateFile->version && EN_BETAVERSION > 0) )
		VersionUpdates = 1;
	else
	//to make the if short :
	// - beta updates should be enabled
	// - the current betaversion should be less then the online beta
	// - the current version +1 should == the beta OR the version == the beta IF a beta is installed
	if ( 
		SGetSetting(SETTING_SHOWBETAUPDATES) && 
		(EN_BETAVERSION < UpdateFile->beta_number) && 
		( ( (VERSION) +1 == UpdateFile->beta_version && EN_BETAVERSION == 0 ) || ( VERSION == UpdateFile->beta_version && EN_BETAVERSION > 0 ) ) 
		)
	{
		BetaUpdates = 1;
	}

	while(1)
	{
		if(redraw)
		{
			if ( VersionUpdates )
			{
				PrintFormat( cur_off == 0, 16, 64+(16*1), "Update to %d.%d",UpdateFile->version >> 8,UpdateFile->version&0xFF);
			}
			else
			{
				PrintFormat( cur_off == 0, 16, 64+(16*1), "No official update\n");
			}
			if (SGetSetting(SETTING_SHOWBETAUPDATES))
			{
				
				if ( BetaUpdates )
				{
					PrintFormat( cur_off==1, 16, 64+(16*2), "Update to %d.%d beta %d",UpdateFile->beta_version >> 8,UpdateFile->beta_version&0xFF, UpdateFile->beta_number);
				}
				else
				{
					PrintFormat( cur_off==1, 16, 64+(16*2), "No Beta update\n");
				}
				PrintFormat( 0, ((rmode->viWidth /2)-((strlen("--------------------"))*13/2))>>1, 64+(16*4), "--------------------");
				PrintFormat( cur_off==2, 16, 64+(16*6), "Other Languages --->\n");
			}
			else
			{
				PrintFormat( 0, ((rmode->viWidth /2)-((strlen("--------------------"))*13/2))>>1, 64+(16*3), "--------------------");
				PrintFormat( cur_off==1, 16, 64+(16*5), "Other Languages --->\n");
			}
			


			PrintFormat( 0, ((rmode->viWidth /2)-((strlen("A(A) Download Update       "))*13/2))>>1, rmode->viHeight-48, "A(A) Download Update       ");
			PrintFormat( 0, ((rmode->viWidth /2)-((strlen("B(B) Cancel Update         "))*13/2))>>1, rmode->viHeight-32, "B(B) Cancel Update         ");
			redraw = 0;
		}
		WPAD_ScanPads();
		PAD_ScanPads();

		u32 WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
		u32 PAD_Pressed  = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);

		if ( WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B )
		{
			free_pointer(UpdateFile);
			return;
		}
		else if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
		{
			if(cur_off == 0 && VersionUpdates == 1)
			{
				DownloadedBeta = 0;
				break;
			}
			else if (cur_off == 1 && BetaUpdates == 1)
			{
				DownloadedBeta = 1;
				break;
			}
			else if( (SGetSetting(SETTING_SHOWBETAUPDATES) && cur_off == 2) || (!SGetSetting(SETTING_SHOWBETAUPDATES) && cur_off == 1) )
			{
				ClearScreen();
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("NOTE:other languages are not officially supported"))*13/2))>>1, 192, "NOTE: other languages are not officially supported");
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("they could have bugs the original doesnt"))*13/2))>>1, 208, "they could have bugs the original doesnt");
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("check readme.txt for their websites"))*13/2))>>1, 224, "check readme.txt for their websites");
				PrintFormat( 1, ((rmode->viWidth /2)-((strlen("Press A to continue or B to bail out"))*13/2))>>1, 224+16, "Press A to continue or B to bail out");
				while(1)
				{
					WPAD_ScanPads();
					PAD_ScanPads();

					WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
					PAD_Pressed  = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);
					if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
					{
						Language = 1;
						break;
					}
					else if ( WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B )
					{
						ClearScreen();
						redraw = 1;
						break;
					}
				}
				if(Language == 1)
				{
					ClearScreen();
					Language = 0;
					s8 failure = 0;
					s8 LangUpdate = 0;
					s8 LangBetaUpdate = 0;
					//german : http://priiloader.baduncles.de/update/version.dat
					//french : none 
					//spanish : none
					
					//download all the updatefiles o.o;
					PrintFormat( 1, ((rmode->viWidth /2)-((strlen("checking for updates..."))*13/2))>>1, 208, "checking for updates...");
					//repeat below code for french and spanish once i get them.
					//every added language should be a power of 2 more. example : german is +=1 , french +=2 , spanish +=4, 4th language +=8, 5th +=16 etc etc
					//this way we can easily tell what language failed( or succeeded ) by the numbers like chmod (bitwise & ) :)
					//info : http://catcode.com/teachmod/numeric.html
					//since we dont have them yet ill set them as autofailure :)
					failure = 6;
					s32 file_size = GetHTTPFile("priiloader.baduncles.de","/update/version.dat",(u8*&)UpdateGer,0);
					if(file_size < 0)
					{
						failure += 1;	
					}
					//add download other languages' version.dat here
					


					//check if any updates are available
					if(failure & 1)
					{
						gprintf("german mod version.dat failed to download\n");
					}
					else
					{
						//it downloaded, lets check for updates.
						if ( (VERSION < UpdateGer->version) || (VERSION == UpdateGer->version && GER_BETAVERSION > 0) )
							LangUpdate += 1;
						else if ( 
							SGetSetting(SETTING_SHOWBETAUPDATES) && 
							(GER_BETAVERSION < UpdateGer->beta_number) && 
							( ( (VERSION) +1 == UpdateGer->beta_version && GER_BETAVERSION == 0 ) || ( VERSION == UpdateGer->beta_version && GER_BETAVERSION > 0 ) || (VERSION == UpdateGer->beta_version && BETAVERSION > 0 ) )
							)
						{
							LangBetaUpdate+= 1;
						}
					}
					if(failure & 2)
					{
						gprintf("French mod version.dat failed to download\n");
					}
					else
					{
						//it downloaded, lets check for updates.
						if ( (VERSION < UpdateFr->version) || (VERSION == UpdateFr->version && FR_BETAVERSION > 0) )
							LangUpdate += 2;
						else if ( 
							SGetSetting(SETTING_SHOWBETAUPDATES) && 
							(FR_BETAVERSION < UpdateFr->beta_number) && 
							( ( (VERSION) +1 == UpdateFr->beta_version && FR_BETAVERSION == 0 ) || ( VERSION == UpdateFr->beta_version && FR_BETAVERSION > 0 ) || (VERSION == UpdateFr->beta_version && BETAVERSION > 0 ) )
							)
						{
							LangBetaUpdate+= 2;
						}
					}
					if (failure & 4)
					{
						gprintf("Spanish mod version.dat failed to download\n");
					}
					else
					{
						if ( (VERSION < UpdateSp->version) || (VERSION == UpdateSp->version && SP_BETAVERSION > 0) )
							LangUpdate += 4;
						else if ( 
							SGetSetting(SETTING_SHOWBETAUPDATES) && 
							(SP_BETAVERSION < UpdateSp->beta_number) && 
							( ( (VERSION) +1 == UpdateSp->beta_version && SP_BETAVERSION == 0 ) || ( VERSION == UpdateSp->beta_version && SP_BETAVERSION > 0 ) || (VERSION == UpdateSp->beta_version && BETAVERSION > 0 ) )
							)
						{
							LangBetaUpdate+= 4;
						}
					}
					if(LangUpdate != 0 || LangBetaUpdate != 0)
					{
						cur_off = 0;
						ClearScreen();
						redraw = 1;
						while(1)
						{
							if(redraw)
							{
								//german
								if(LangUpdate & 1)
								{
									PrintFormat( cur_off == 0, 16, 64+(16*1), "Update to %d.%d(german)",UpdateGer->version >> 8,UpdateGer->version&0xFF);
								}
								else
								{
									PrintFormat( cur_off == 0, 16, 64+(16*1), "No German version Update");
								}
								if(LangBetaUpdate & 1)
								{
									PrintFormat( cur_off == 1, 16, 64+(16*2), "Update to %d.%d(german beta %d)",UpdateGer->beta_version >> 8,UpdateGer->beta_version&0xFF,UpdateGer->beta_number);
								}
								else
								{
									PrintFormat( cur_off == 1, 16, 64+(16*2), "No German Beta Updates");
								}
								//french
								if(LangUpdate & 2)
								{
									PrintFormat( cur_off == 2, 16, 64+(16*4), "Update to %d.%d(french)",UpdateFr->version >> 8,UpdateFr->version&0xFF);
								}
								else
								{
									PrintFormat( cur_off == 2, 16, 64+(16*4), "No French version Update");
								}
								if(LangBetaUpdate & 2)
								{
									PrintFormat( cur_off == 3, 16, 64+(16*5), "Update to %d.%d(french beta %d)",UpdateFr->version >> 8,UpdateFr->version&0xFF,UpdateFr->beta_number);
								}
								else
								{
									PrintFormat( cur_off == 4, 16, 64+(16*5), "No French Beta Updates");
								}
								//spanish (if it will even get done some day lol)
								if(LangUpdate & 4)
								{
									PrintFormat( cur_off == 5, 16, 64+(16*7), "Update to %d.%d(spanish)",UpdateSp->version >> 8,UpdateSp->version&0xFF);
								}
								else
								{
									PrintFormat( cur_off == 5, 16, 64+(16*7), "No Spanish version Update");
								}
								if(LangBetaUpdate & 4)
								{
									PrintFormat( cur_off == 6, 16, 64+(16*8), "Update to %d.%d(spanish beta %d)",UpdateSp->version >> 8,UpdateSp->version&0xFF,UpdateSp->beta_number);
								}
								else
								{
									PrintFormat( cur_off == 6, 16, 64+(16*8), "No Spanish Beta Updates");
								}
								redraw = 0;
							}
							WPAD_ScanPads();
							PAD_ScanPads();

							WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
							PAD_Pressed  = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);
							if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
							{
								//repeative code i know. sorry. the whole language part needs fucking clean up
								switch(cur_off)
								{
								case 0: //german mod - update
									if(LangUpdate & 1)
									{
										Language = cur_off +1;
									}
									break;
								case 1: //german mod - beta
									if(LangBetaUpdate & 1)
									{
										Language = cur_off +1;
									}
									break;
								case 2: // french mod - update
									if(LangUpdate & 2)
									{
										Language = cur_off +1;
									}
									break;
								case 3: // french mod - beta
									if(LangBetaUpdate & 2)
									{
										Language = cur_off +1;
									}
									break;
								case 4: // french mod - update
									if(LangUpdate & 4)
									{
										Language = cur_off +1;
									}
									break;
								case 5: // french mod - beta
									if(LangBetaUpdate & 4)
									{
										Language = cur_off +1;
									}
									break;
								default:
									break;
								}
								if(Language != 0)
									break;
								redraw = 1;
							}
							else if ( WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B )
							{
								ClearScreen();
								break;
							}
							else if ( WPAD_Pressed & WPAD_BUTTON_UP || WPAD_Pressed & WPAD_CLASSIC_BUTTON_UP || PAD_Pressed & PAD_BUTTON_UP )
							{
								cur_off--;
								if(cur_off < 0)
									cur_off = 6;
								redraw = 1;
							}
							else if ( WPAD_Pressed & WPAD_BUTTON_DOWN || WPAD_Pressed & WPAD_CLASSIC_BUTTON_DOWN || PAD_Pressed & PAD_BUTTON_DOWN )
							{
								cur_off++;
								if(cur_off > 6)
									cur_off = 0;
								redraw = 1;
							}
						} // exit while of list language updates
						redraw = 1;
						if(Language != 0)
							break;	
						else
						{
							//reset everything from language stuff. we are going back to the main menu :')
							if(UpdateGer)
								free_pointer(UpdateGer);
							if(UpdateFr)
								free_pointer(UpdateFr);
							if(UpdateSp)
								free_pointer(UpdateSp);
							cur_off = 0;
						}
					}
					else
					{
						PrintFormat( 1, ((rmode->viWidth /2)-((strlen("no updates found"))*13/2))>>1, 224, "no updates found");
						sleep(2);
						if(UpdateGer)
							free_pointer(UpdateGer);
						if(UpdateFr)
							free_pointer(UpdateFr);
						if(UpdateSp)
							free_pointer(UpdateSp);
						//no updates o.o;

					}
					redraw = 1;
				} //exit if of if they agreed or not
			} // exit whole if it s a language they want
			redraw = 1;
		}
		else if ( WPAD_Pressed & WPAD_BUTTON_UP || WPAD_Pressed & WPAD_CLASSIC_BUTTON_UP || PAD_Pressed & PAD_BUTTON_UP )
		{
			cur_off--;
			if(cur_off < 0)
			{
				if (SGetSetting(SETTING_SHOWBETAUPDATES))
				{	
					
						cur_off = 2;			
				}
				else
				{
						cur_off = 1;	
				}
			}
			redraw = 1;
		}
		else if ( WPAD_Pressed & WPAD_BUTTON_DOWN || WPAD_Pressed & WPAD_CLASSIC_BUTTON_DOWN || PAD_Pressed & PAD_BUTTON_DOWN )
		{
			cur_off++;
			if (SGetSetting(SETTING_SHOWBETAUPDATES))
			{
				if(cur_off > 2)
					cur_off = 0;
			}
			else
			{
				if(cur_off > 1)
					cur_off = 0;
			}
			redraw = 1;
		}
	}
//Download changelog and ask to proceed or not
//------------------------------------------------------
	gprintf("downloading changelog...\n");
	if (Language == 0)
	{
		if(DownloadedBeta)
		{
			file_size = GetHTTPFile("www.dacotaco.com","/priiloader/changelog_beta.txt",Changelog,0);
		}
		else
		{
			file_size = GetHTTPFile("www.dacotaco.com","/priiloader/changelog.txt",Changelog,0);
		}
	}
	else
	{
		switch(Language)
		{
			case 1:
				//http://priiloader.baduncles.de/update/changelog.txt
				file_size = GetHTTPFile("priiloader.baduncles.de","/update/changelog.txt",Changelog,0);
				break;
			case 2:
				file_size = GetHTTPFile("priiloader.baduncles.de","/update/changelog_beta.txt",Changelog,0);
				break;
			case 3:
				file_size = -1;
				break;
			case 4:
				file_size = -1;
				break;
			case 5:
				file_size = -1;
				break;
			case 6:
				file_size = -1;
				break;
			default:
				file_size = -1;
				break;
		}
	}
	if (file_size > 0)
	{
		ClearScreen();
		char se[5];
		u8 line = 0;
		u8 min_line = 0;
		u8 max_line = 0;
		if( rmode->viTVMode == VI_NTSC || CONF_GetEuRGB60() || CONF_GetProgressiveScan() )
			max_line = 14;
		else
			max_line = 19;
		u8 redraw = 1;

		char *ptr;
		std::vector<char*> lines;

		if( strpbrk((char*)Changelog , "\r\n") )
			sprintf(se, "\r\n");
		else
			sprintf(se, "\n");
		ptr = strtok((char*)Changelog, se);
		while (ptr != NULL)
		{
			lines.push_back(ptr);
			ptr = strtok (NULL, se);
		}
		free_pointer(Changelog);
		if( max_line >= lines.size() )
			max_line = lines.size()-1;

		PrintFormat( 1, ((rmode->viWidth /2)-((strlen(" Changelog "))*13/2))>>1, 64+(16*1), " Changelog ");
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("-----------"))*13/2))>>1, 64+(16*2), "-----------");
		if((lines.size() -1) > max_line)
		{
			PrintFormat( 0, ((rmode->viWidth /2)-((strlen("Up    Scroll Up        "))*13/2))>>1, rmode->viHeight-80, "Up    Scroll Up");
			PrintFormat( 0, ((rmode->viWidth /2)-((strlen("Down  Scroll Down      "))*13/2))>>1, rmode->viHeight-64, "Down  Scroll Down");
		}
		PrintFormat( 0, ((rmode->viWidth /2)-((strlen("A(A)  Proceed(Download)"))*13/2))>>1, rmode->viHeight-48, "A(A)  Proceed(Download)");
		PrintFormat( 0, ((rmode->viWidth /2)-((strlen("B(B)  Cancel Update    "))*13/2))>>1, rmode->viHeight-32, "B(B)  Cancel Update    ");
		u32 PAD_Pressed = 0;
		u32 WPAD_Pressed = 0;
		while(1)
		{
			WPAD_ScanPads();
			PAD_ScanPads();

			WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
			PAD_Pressed  = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);
			if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
			{
				break;
			}
			if ( WPAD_Pressed & WPAD_BUTTON_B || WPAD_Pressed & WPAD_CLASSIC_BUTTON_B || PAD_Pressed & PAD_BUTTON_B )
			{
				free_pointer(UpdateFile);
				if (Language > 0)
				{
					if(UpdateGer)
						free_pointer(UpdateGer);
					if(UpdateFr)
						free_pointer(UpdateFr);
					if(UpdateSp)
						free_pointer(UpdateSp);
				}
				ClearScreen();
				return;
			}
			if ( WPAD_Pressed & WPAD_BUTTON_DOWN || WPAD_Pressed & WPAD_CLASSIC_BUTTON_DOWN || PAD_Pressed & PAD_BUTTON_DOWN )
			{
				if ( (min_line+max_line) < lines.size()-1 )
				{
					min_line++;
					redraw = true;
				}
			}
			if ( WPAD_Pressed & WPAD_BUTTON_UP || WPAD_Pressed & WPAD_CLASSIC_BUTTON_UP || PAD_Pressed & PAD_BUTTON_UP )
			{
				if ( min_line > 0 )
				{
					min_line--;
					redraw = true;
				}
			}
			if ( redraw )
			{
				for(u8 i = min_line; i <= (min_line+max_line); i++)
				{
					PrintFormat( 0, 16, 64 + ((line+3)*16), "                                                                ");
					PrintFormat( 0, 16, 64 + ((line+3)*16), "%s", lines[i]);
					if( i < lines.size()-1 && i == (min_line+max_line) )
						PrintFormat( 0, 16, 64 + ((line+4)*16), "...");
					else
						PrintFormat( 0, 16, 64 + ((line+4)*16), "   ");
					line++;
				}
				redraw = false;
				line = 0;
			}
		}
	}
	else if(file_size < 0)
	{
		if(file_size < -9)
			free_pointer(Changelog);
		gprintf("CheckForUpdate : failed to get changelog.error %d, HTTP reply %s\n",file_size,Get_Last_reply());
	}
//Check Pad for lulz or not
//-----------------------------
	file_size = 0;
	u8 *Easter_egg = NULL;
	PAD_ScanPads();
	WPAD_ScanPads();
	u32 PAD_Pressed  = PAD_ButtonsHeld(0) | PAD_ButtonsHeld(1) | PAD_ButtonsHeld(2) | PAD_ButtonsHeld(3);// | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3); //PAD_ButtonsHeld
	if (PAD_Pressed & PAD_TRIGGER_Z)
	{
		file_size = GetHTTPFile("www.dacotaco.com","/priiloader/Easter.mp3",Easter_egg,0);
		if(file_size)
		{
			ASND_Init();
			ASND_Pause(0);
			MP3Player_Init();
			MP3Player_Volume(125);
			MP3Player_PlayBuffer(Easter_egg,file_size,NULL);
			ClearScreen();
			PrintFormat( 1, ((640/2)-((strlen("MOOT MOOT ;D"))*13/2))>>1, 208, "MOOT MOOT ;D");
			while(MP3Player_IsPlaying())
				sleep(1);
			ASND_End();
		}
	}
	free_pointer(Easter_egg);
//The choice is made. lets download what the user wanted :)
//--------------------------------------------------------------

//if a language was downloaded we shall place all the data of the selected lang into UpdateFile. and if a beta was chosen update DownloadedBeta with the info
	if(Language != 0)
	{
		if(UpdateFile)
			free_pointer(UpdateFile);
		switch(Language)
		{
		case 2:
			DownloadedBeta = 1;
		case 1:
			UpdateFile = UpdateGer;
			break;
		case 4:
			DownloadedBeta = 1;
		case 3:
			UpdateFile = UpdateFr;
			break;
		case 6:
			DownloadedBeta = 1;
		case 5:
			UpdateFile = UpdateSp;
			break;
		}
	}

	ClearScreen();
	if (Language == 0)
	{
		gprintf("downloading %s\n",DownloadedBeta?"beta":"update");
		if(DownloadedBeta)
		{
			PrintFormat( 1, ((640/2)-((strlen("downloading   .   beta   ..."))*13/2))>>1, 208, "downloading %d.%d beta %d...",UpdateFile->beta_version >> 8,UpdateFile->beta_version&0xFF, UpdateFile->beta_number);
			file_size = GetHTTPFile("www.dacotaco.com","/priiloader/Priiloader_Beta.dol",Data,0);
			//download beta
		}
		else
		{
			PrintFormat( 1, ((640/2)-((strlen("downloading   .  ..."))*13/2))>>1, 208, "downloading %d.%d ...",UpdateFile->version >> 8,UpdateFile->version&0xFF);
			file_size = GetHTTPFile("www.dacotaco.com","/priiloader/Priiloader_Update.dol",Data,0);
			//download Update
		}
	}
	else
	{
		gprintf("downloading language mod...\n");
		PrintFormat( 1, ((640/2)-((strlen("Downloading Language mod update..."))*13/2))>>1, 208, "Downloading Language mod update...");
		switch(Language)
		{
			case 1:
				//http://priiloader.baduncles.de/update/Priiloader_Beta.dol
				file_size = GetHTTPFile("priiloader.baduncles.de","/update/Priiloader.dol",Data,0);
				break;
			case 2:
				file_size = GetHTTPFile("priiloader.baduncles.de","/update/Priiloader_Beta.dol",Data,0);
				break;
			case 3:
				file_size = -1;
				break;
			case 4:
				file_size = -1;
				break;
			case 5:
				file_size = -1;
				break;
			case 6:
				file_size = -1;
				break;
		}
	}
	if ( file_size <= 0 )
	{
		if (file_size < 0 && file_size > -4)
		{
			//errors connecting to server
			gprintf("connection failure: error %d\n",file_size);
		}
		else if (file_size == -7)
		{
			gprintf("HTTP Error %s!\n",Get_Last_reply());
		}
		else
		{
			if(file_size < -9)
				free_pointer(Data);
			gprintf("getting update error %d\n",file_size);
		}
		PrintFormat( 1, ((rmode->viWidth /2)-((strlen("error getting file from server"))*13/2))>>1, 224, "error getting file from server");
		sleep(2);
		free_pointer(UpdateFile);
		return;
	}
	else
	{
		SHA1* sha1 = new SHA1();
		sha1->addBytes( (char*)Data, file_size );

		u32 FileHash[5];
		gprintf("Downloaded update ");
		sha1->getDigest(FileHash,NULL);
		sha1->hexPrinter_array(FileHash);
		gprintf("Online ");
		if (!DownloadedBeta)
			sha1->hexPrinter_array(UpdateFile->SHA1_Hash);
		else
			sha1->hexPrinter_array(UpdateFile->beta_SHA1_Hash);
		delete sha1;

		if (
			( !DownloadedBeta && (
			UpdateFile->SHA1_Hash[0] != FileHash[0] ||
			UpdateFile->SHA1_Hash[1] != FileHash[1] ||
			UpdateFile->SHA1_Hash[2] != FileHash[2] ||
			UpdateFile->SHA1_Hash[3] != FileHash[3] ||
			UpdateFile->SHA1_Hash[4] != FileHash[4] ) ) ||

			( DownloadedBeta && (
			UpdateFile->beta_SHA1_Hash[0] != FileHash[0] ||
			UpdateFile->beta_SHA1_Hash[1] != FileHash[1] ||
			UpdateFile->beta_SHA1_Hash[2] != FileHash[2] ||
			UpdateFile->beta_SHA1_Hash[3] != FileHash[3] ||
			UpdateFile->beta_SHA1_Hash[4] != FileHash[4] ) ) )
		{
			gprintf("File not the same : hash check failure!\n");
			PrintFormat( 1, ((640/2)-((strlen("Error Downloading Update"))*13/2))>>1, 224, "Error Downloading Update");
			sleep(5);
			free_pointer(UpdateFile);
			free_pointer(Data);
			return;
		}
		else
		{
			gprintf("Hash check complete. booting file...\n");
		}

//Load the dol
//---------------------------------------------------
		ClearScreen();
		if (Language == 0)
		{
			if(DownloadedBeta)
			{
				PrintFormat( 1, ((640/2)-((strlen("loading   .   beta   ..."))*13/2))>>1, 208, "loading %d.%d beta %d...",UpdateFile->beta_version >> 8,UpdateFile->beta_version&0xFF, UpdateFile->beta_number);
			}
			else
			{
				PrintFormat( 1, ((640/2)-((strlen("loading   .  ..."))*13/2))>>1, 208, "loading %d.%d ...",UpdateFile->version >> 8,UpdateFile->version&0xFF);
			}
		}
		else
		{
			if(DownloadedBeta)
			{
				PrintFormat( 1, ((640/2)-((strlen("loading lang mod   .   beta   ..."))*13/2))>>1, 208, "loading lang mod %d.%d beta %d...",UpdateFile->beta_version >> 8,UpdateFile->beta_version&0xFF, UpdateFile->beta_number);
			}
			else
			{
				PrintFormat( 1, ((640/2)-((strlen("loading lang mod  .  ..."))*13/2))>>1, 208, "loading lang mod %d.%d ...",UpdateFile->version >> 8,UpdateFile->version&0xFF);
			}
		}
		free_pointer(UpdateFile);
		sleep(1);
		//load the fresh installer
		BootDolFromMem(Data);
		free_pointer(Data);
		PrintFormat( 1, ((640/2)-((strlen("Error Booting Update dol"))*13/2))>>1, 224, "Error Booting Update dol");
		sleep(5);
	}
	return;
}
void HandleSTMEvent(u32 event)
{
	f64 ontime;
	switch(event)
	{
		case STM_EVENT_POWER:
			Shutdown=1;
			BootSysMenu = 0;
			break;
		case STM_EVENT_RESET:
			if (BootSysMenu == 0 && WPAD_Probe(0,0) < 0)
			{
				time_t inloop;
				time(&inloop);
				ontime = difftime(inloop, startloop);
				gprintf("ontime = %4.2fs\n",ontime);
				if (ontime >= 15)
					BootSysMenu = 1;
			}
		default:
			break;
	}
}
void Autoboot_System( void )
{
  	if( SGetSetting(SETTING_PASSCHECKMENU) && SGetSetting(SETTING_AUTBOOT) != AUTOBOOT_DISABLED && SGetSetting(SETTING_AUTBOOT) != AUTOBOOT_ERROR )
 		password_check();

	switch( SGetSetting(SETTING_AUTBOOT) )
	{
		case AUTOBOOT_SYS:
			gprintf("AutoBoot:System Menu\n");
			BootMainSysMenu(0);
			break;
		case AUTOBOOT_HBC:
			gprintf("AutoBoot:Homebrew Channel\n");
			LoadHBC();
			break;
		case AUTOBOOT_BOOTMII_IOS:
			gprintf("AutoBoot:BootMii IOS\n");
			LoadBootMii();
			error=ERROR_BOOT_BOOTMII;
			break;
		case AUTOBOOT_FILE:
			gprintf("AutoBoot:Installed File\n");
			AutoBootDol();
			break;
		case AUTOBOOT_ERROR:
			error=ERROR_BOOT_ERROR;
		case AUTOBOOT_DISABLED:
		default:
			break;
	}
	return;
}
int main(int argc, char **argv)
{
	CheckForGecko();
#ifdef DEBUG
	InitGDBDebug();
#endif
	gprintf("priiloader\n");
	gprintf("Built   : %s %s\n", __DATE__, __TIME__ );
	gprintf("Version : %d.%dd (rev %s)\n", VERSION>>16, VERSION&0xFFFF, SVN_REV_STR);
	gprintf("Firmware: %d.%d.%d\n", *(vu16*)0x80003140, *(vu8*)0x80003142, *(vu8*)0x80003143 );

	*(vu32*)0x80000020 = 0x0D15EA5E;				// Magic word (how did the console boot?)
	*(vu32*)0x800000F8 = 0x0E7BE2C0;				// Bus Clock Speed
	*(vu32*)0x800000FC = 0x2B73A840;				// CPU Clock Speed

	*(vu32*)0x80000040 = 0x00000000;				// Debugger present?
	*(vu32*)0x80000044 = 0x00000000;				// Debugger Exception mask
	*(vu32*)0x80000048 = 0x00000000;				// Exception hook destination 
	*(vu32*)0x8000004C = 0x00000000;				// Temp for LR
	*(vu32*)0x80003100 = 0x01800000;				// Physical Mem1 Size
	*(vu32*)0x80003104 = 0x01800000;				// Console Simulated Mem1 Size

	*(vu32*)0x80003118 = 0x04000000;				// Physical Mem2 Size
	*(vu32*)0x8000311C = 0x04000000;				// Console Simulated Mem2 Size

	*(vu32*)0x80003120 = 0x93400000;				// MEM2 end address ?

	s32 r = ISFS_Initialize();
	if( r < 0 )
	{
		*(vu32*)0xCD8000C0 |= 0x20;
		error=ERROR_ISFS_INIT;
	}

	LoadHBCStub();
	gprintf("\"Magic Priiloader word\": %x\n",*(vu32*)0x8132FFFB);
	LoadSettings();
	SetShowDebug(SGetSetting(SETTING_SHOWGECKOTEXT));
	if ( SGetSetting(SETTING_SHOWGECKOTEXT) != 0 )
	{
		InitVideo();
	}

	s16 Bootstate = CheckBootState();
	gprintf("BootState:%d\n", Bootstate );
	//Check reset button state
	//TODO : move magic word handling to some place else (its own function?)
	if( ((*(vu32*)0xCC003000)>>16)&1 && *(vu32*)0x8132FFFB != 0x4461636f && *(vu32*)0x8132FFFB != 0x50756e65) //0x4461636f = "Daco" in hex, 0x50756e65 = "Pune"
	{
		//Check autoboot settings
		StateFlags temp;
		switch( Bootstate )
		{
			case TYPE_UNKNOWN: //255 or -1, only seen when shutting down from MIOS or booting dol from HBC. it is actually an invalid value
				temp = GetStateFlags();
				gprintf("Bootstate %u detected. DiscState %u ,ReturnTo %u & Flags %u\n",temp.type,temp.discstate,temp.returnto,temp.flags);
				if( temp.flags == 130 ) //&& temp.discstate != 2)
				{
					//if the flag is 130, its probably shutdown from mios. in that case system menu 
					//will handle it perfectly (and i quote from SM's OSreport : "Shutdown system from GC!")
					//it seems to reboot into bootstate 5. but its safer to let SM handle it
					gprintf("255:System Menu\n");
					BootMainSysMenu(0);
				}
				else
				{
					Autoboot_System();
				}
				break;
			case TYPE_SHUTDOWNSYSTEM: // 5 - shutdown
				if( ClearState() < 0 )
				{
					gprintf("ClearState failure\n");
				}
				//check for valid nandboot shitzle. if its found we need to change bootstate to 4.
				//yellow8 claims system menu reset everything then, but it didn't on my wii (joy). this is why its not activated code.
				//its not fully confirmed system menu does it(or ios while being standby) and if system menu does indeed clear it.
				/*if(VerifyNandBootInfo())
				{
					gprintf("Verifty of NandBootInfo : 1\nbootstate changed to %d\n",CheckBootState());
				}
				else
				{
					gprintf("Verifty of NandBootInfo : 0\n");
				}*/
				if(!SGetSetting(SETTING_SHUTDOWNTOPRELOADER))
				{
					gprintf("Shutting down...\n");
					*(vu32*)0xCD8000C0 &= ~0x20;
					Control_VI_Regs(0);
					DVDStopDisc(false);
        			WPAD_Shutdown();
					ShutdownDevices();
					USB_Deinitialize();
					if( SGetSetting(SETTING_IGNORESHUTDOWNMODE) )
					{
						STM_ShutdownToStandby();

					} else {
						if( CONF_GetShutdownMode() == CONF_SHUTDOWN_STANDBY )
						{
							//standby = red = off							
							STM_ShutdownToStandby();
						}
						else
						{
							//idle = orange = standby
							s32 ret;
							ret = CONF_GetIdleLedMode();
							if (ret >= 0 && ret <= 2)
								STM_SetLedMode(ret);
							STM_ShutdownToIdle();
						}
					}
				}
				break;
			case RETURN_TO_ARGS: //2 - normal reboot which funny enough doesn't happen very often
			case TYPE_RETURN: //3 - return to system menu
				switch( SGetSetting(SETTING_RETURNTO) )
				{
					case RETURNTO_SYSMENU:
						gprintf("ReturnTo:System Menu\n");
						BootMainSysMenu(0);
					break;

					case RETURNTO_AUTOBOOT:
						Autoboot_System();
					break;

					default:
					break;
				}
				break;
			case TYPE_NANDBOOT: // 4 - nandboot
				//apparently a boot state in which the system menu auto boots a title. read more : http://wiibrew.org/wiki/WiiConnect24#WC24_title_booting
				//as it hardly happens i guess nothing bad can happen if we ignore it and just do autoboot instead :)
			case RETURN_TO_SETTINGS: // 1 - Boot when fully shutdown & wiiconnect24 is off. why its called RETURN_TO_SETTINGS i have no clue...
			case RETURN_TO_MENU: // 0 - boot when wiiconnect24 is on
				Autoboot_System();
				break;
			default :
				if( ClearState() < 0 )
				{
					error = ERROR_STATE_CLEAR;
					gprintf("ClearState failure\n");
				}
				break;

		}
	}
	//remove the "Magic Priiloader word" cause it has done its purpose
	if(*(vu32*)0x8132FFFB == 0x4461636f)
 	{
		gprintf("\"Magic Priiloader Word\" 'Daco' found!\n");
		gprintf("clearing memory of the \"Magic Priiloader Word\"\n");
		*(vu32*)0x8132FFFB = 0x00000000;
		DCFlushRange((void*)0x8132FFFB,4);
	}
	else if(*(vu32*)0x8132FFFB == 0x50756e65)
	{
		//detected the force for sys menu
		gprintf("\"Magic Priiloader Word\" 'Pune' found!\n");
		gprintf("clearing memory of the \"Magic Priiloader Word\" and starting system menu...\n");
		*(vu32*)0x8132FFFB = 0x00000000;
		DCFlushRange((void*)0x8132FFFB,4);
		BootMainSysMenu(0);
	}
	else if( 
			( SGetSetting(SETTING_AUTBOOT) != AUTOBOOT_DISABLED && ( Bootstate < 2 || Bootstate == 4 ) ) 
		 || ( SGetSetting(SETTING_RETURNTO) != RETURNTO_PRELOADER && Bootstate > 1 && Bootstate != 4 ) 
		 || ( SGetSetting(SETTING_SHUTDOWNTOPRELOADER) == 0 && Bootstate == 5 ) 
		)
	{
		gprintf("Reset Button is held down\n");
	}
	
	if ( SGetSetting(SETTING_SHOWGECKOTEXT) == 0 )
	{
		//init video first so we can see crashes :)
		InitVideo();
	}
  	if( SGetSetting(SETTING_PASSCHECKPRII) )
 		password_check();

	AUDIO_Init (NULL);
	DSP_Init ();
	AUDIO_StopDMA();
	AUDIO_RegisterDMACallback(NULL);

	r = (s32)PollDevices();
	gprintf("FAT_Init():%d\n", r );

	r = PAD_Init();
	gprintf("PAD_Init():%d\n", r );

	r = WPAD_Init();
	gprintf("WPAD_Init():%d\n", r );

	WPAD_SetPowerButtonCallback(HandleWiiMoteEvent);
	STM_RegisterEventHandler(HandleSTMEvent);

	ClearScreen();

	s32 cur_off=0;
	u32 redraw=true;
	u32 SysVersion=GetSysMenuVersion();

	if( SGetSetting(SETTING_STOPDISC) )
	{
		DVDStopDisc(false);
	}
	time(&startloop);

	while(1)
	{
		WPAD_ScanPads();
		PAD_ScanPads();

		u32 WPAD_Pressed = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
		u32 PAD_Pressed  = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);
 
#ifdef DEBUG
		if ( (WPAD_Pressed & WPAD_BUTTON_HOME) || (PAD_Pressed & PAD_BUTTON_START) )
		{
			
			
			LoadHacks();
			//u64 TitleID = 0x0001000030303032LL;

			//u32 cnt ATTRIBUTE_ALIGN(32);
			//ES_GetNumTicketViews(TitleID, &cnt);
			//tikview *views = (tikview *)memalign( 32, sizeof(tikview)*cnt );
			//ES_GetTicketViews(TitleID, views, cnt);
			//ES_LaunchTitle(TitleID, &views[0]);	
		}
#endif

		if ( WPAD_Pressed & WPAD_BUTTON_A || WPAD_Pressed & WPAD_CLASSIC_BUTTON_A || PAD_Pressed & PAD_BUTTON_A )
		{
			ClearScreen();
			switch(cur_off)
			{
				case 0:
					BootMainSysMenu(1);
					if(!error)
						error=ERROR_SYSMENU_GENERAL;
					break;
				case 1:		//Load HBC
					LoadHBC();
					break;
				case 2: //Load Bootmii
					LoadBootMii();
					//well that failed...
					error=ERROR_BOOT_BOOTMII;
					break;
				case 3: // show titles list
					LoadListTitles();
					break;
				case 4:		//load main.bin from /title/00000001/00000002/data/ dir
					AutoBootDol();
					break;
				case 5:
					InstallLoadDOL();
					break;
				case 6:
					if( SGetSetting( SETTING_CLASSIC_HACKS ) )
						SysHackSettings();
					else
						SysHackHashSettings();
					break;
				case 7:
					CheckForUpdate();
					break;
				case 8:
					InstallPassword();
					break;
				case 9:
					SetSettings();
					break;
				default:
					break;

			}

			ClearScreen();
			redraw=true;
		}

		if ( WPAD_Pressed & WPAD_BUTTON_DOWN || WPAD_Pressed & WPAD_CLASSIC_BUTTON_DOWN || PAD_Pressed & PAD_BUTTON_DOWN )
		{
			cur_off++;

			if( error == ERROR_UPDATE )
			{
				if( cur_off >= 11 )
					cur_off = 0;
			}else {

				if( cur_off >= 10 )
					cur_off = 0;
			}

			redraw=true;
		} else if ( WPAD_Pressed & WPAD_BUTTON_UP || WPAD_Pressed & WPAD_CLASSIC_BUTTON_UP || PAD_Pressed & PAD_BUTTON_UP )
		{
			cur_off--;

			if( cur_off < 0 )
			{
				if( error == ERROR_UPDATE )
				{
					cur_off=11-1;
				} else {
					cur_off=10-1;
				}
			}

			redraw=true;
		}

		if( redraw )
		{
#ifdef DEBUG
			printf("\x1b[2;0Hpreloader v%d.%d DEBUG (Sys:%d)(IOS:%d)(%s %s)\n", VERSION>>8, VERSION&0xFF, SysVersion, (*(vu32*)0x80003140)>>16, __DATE__, __TIME__);
#else
			if( BETAVERSION > 0 )
			{
				PrintFormat( 0, 160, rmode->viHeight-48, "priiloader v%d.%d(beta v%d)", VERSION>>8, VERSION&0xFF, BETAVERSION&0xFF );
			} else {
				PrintFormat( 0, 160, rmode->viHeight-48, "priiloader v%d.%dd (r%s)", VERSION>>8, VERSION&0xFF,SVN_REV_STR );
			}
			PrintFormat( 0, 16, rmode->viHeight-64, "IOS v%d", (*(vu32*)0x80003140)>>16 );
			PrintFormat( 0, 16, rmode->viHeight-48, "Systemmenu v%d", SysVersion );			
			PrintFormat( 0, 16, rmode->viHeight-20, "Priiloader is a mod of Preloader 0.30");
#endif

			PrintFormat( cur_off==0, ((rmode->viWidth /2)-((strlen("System Menu"))*13/2))>>1, 64, "System Menu");
			PrintFormat( cur_off==1, ((rmode->viWidth /2)-((strlen("Homebrew Channel"))*13/2))>>1, 80, "Homebrew Channel");
			PrintFormat( cur_off==2, ((rmode->viWidth /2)-((strlen("BootMii IOS"))*13/2))>>1, 96, "BootMii IOS");
			PrintFormat( cur_off==3, ((rmode->viWidth /2)-((strlen("Launch Title"))*13/2))>>1, 112, "Launch Title");
			PrintFormat( cur_off==4, ((rmode->viWidth /2)-((strlen("Installed File"))*13/2))>>1, 144, "Installed File");
			PrintFormat( cur_off==5, ((rmode->viWidth /2)-((strlen("Load/Install File"))*13/2))>>1, 160, "Load/Install File");
			PrintFormat( cur_off==6, ((rmode->viWidth /2)-((strlen("System Menu Hacks"))*13/2))>>1, 176, "System Menu Hacks");
			PrintFormat( cur_off==7, ((rmode->viWidth /2)-((strlen("Check For Update"))*13/2))>>1,192,"Check For Update");
			PrintFormat( cur_off==8, ((rmode->viWidth /2)-((strlen("Set Password"))*13/2))>>1, 208, "Set Password");
			PrintFormat( cur_off==9, ((rmode->viWidth /2)-((strlen("Settings"))*13/2))>>1, 224, "Settings");

			if (error > 0)
			{
				ShowError();
				error = ERROR_NONE;
			}
			redraw = false;
		}
		if( Shutdown )
		{
			*(vu32*)0xCD8000C0 &= ~0x20;
			//when we are in preloader itself we should make the video black or de-init it before the user thinks its not shutting down...
			//TODO : fade to black if possible without a gfx lib?
			//STM_SetVIForceDimming ?
			ClearState();
			VIDEO_ClearFrameBuffer( rmode, xfb, COLOR_BLACK);
			Control_VI_Regs(0);
			DVDStopDisc(false);
			WPAD_Shutdown();
			ShutdownDevices();
			USB_Deinitialize();
			if( SGetSetting(SETTING_IGNORESHUTDOWNMODE) )
			{
				STM_ShutdownToStandby();
			} 
			else 
			{
				if( CONF_GetShutdownMode() == CONF_SHUTDOWN_STANDBY )
				{
					//standby = red = off
					STM_ShutdownToStandby();
				}
				else
				{
					//idle = orange = standby
					s32 ret;
					ret = CONF_GetIdleLedMode();
					if (ret >= 0 && ret <= 2)
						STM_SetLedMode(ret);
					STM_ShutdownToIdle();
				}
			}

		}
		//boot system menu
		if(BootSysMenu)
		{
			gprintf("booting main system menu...\n");
			if ( !SGetSetting(SETTING_USESYSTEMMENUIOS) )
			{
				gprintf("Changed Settings to use System Menu IOS...\n");
				settings->UseSystemMenuIOS = true;
			}
			BootMainSysMenu(1);
			if(!error)
				error=ERROR_SYSMENU_GENERAL;
			BootSysMenu = 0;
		}
		//check Mounted Devices
		PollDevices();
		
		VIDEO_WaitVSync();
	}

	return 0;
}
