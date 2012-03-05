#ifndef __FB_H
#define __FB_H

#include <lib/base/eerror.h>
#include <linux/fb.h>

#if defined(__sh__) 
	#include <linux/stmfb.h> 
#endif
class fbClass
{
	int fd;
	unsigned int xRes, yRes, stride, bpp;
#if defined(__sh__) 
	unsigned int xResFB, yResFB; 
	int topDiff, leftDiff, rightDiff, bottomDiff; 
	unsigned char *lfb_direct;
#endif
	int available;
	struct fb_var_screeninfo screeninfo, oldscreen;
	fb_cmap cmap;
	__u16 red[256], green[256], blue[256], trans[256];
	static fbClass *instance;
	int locked;

	int m_manual_blit;
	int m_number_of_pages;
	int m_phys_mem;
#ifdef SWIG
	fbClass(const char *fb="/dev/fb0");
	~fbClass();
public:
#else
public:
	unsigned char *lfb;
	void enableManualBlit();
	void disableManualBlit();
	int showConsole(int state);
	int SetMode(unsigned int xRes, unsigned int yRes, unsigned int bpp);
	int Available() { return available; }
	
	int getNumPages() { return m_number_of_pages; }
	
	unsigned long getPhysAddr() { return m_phys_mem; }
	
	int setOffset(int off);
	int waitVSync();
	void blit();
	unsigned int Stride() { return stride; }
	fb_cmap *CMAP() { return &cmap; }

	fbClass(const char *fb="/dev/fb0");
	~fbClass();
	
			// low level gfx stuff
	int PutCMAP();
#endif
	static fbClass *getInstance();
#if defined(__sh__)  
//---> "hack" for libeplayer3 fb access
        int getFD() { return fd; }
        unsigned char * getLFB_Direct() { return lfb_direct; }
        int getScreenResX() { return xRes; }
        int getScreenResY() { return yRes; }
//---<
#endif  

	int lock();
	void unlock();
	int islocked() { return locked; }
#if defined(__sh__) 
	void clearFBblit();
	int getFBdiff(int ret);
	void setFBdiff(int top, int right, int left, int bottom);
#endif
};

#endif
