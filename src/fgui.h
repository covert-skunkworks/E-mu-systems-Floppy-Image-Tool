//  Description:
//
//		fgui.h -- the program's window.
//

#ifndef FGUI_H
#define FGUI_H

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Set what the pages start with, from the command line.
  *
  * @param  dos 1 for the DOS format on the make page.
  * @param  file A file to add to the make page's list, may be NULL.
  * @param  image The image on the write page, may be NULL.
  * @param  out The image file on the make and read pages, may be NULL.
  *
  * @retval None.
  */
void fgui_preset( int dos, const char *file, const char *image, const char *out );

#ifdef FDR_TEST_DRIVE
/**
  * @brief  Script a click, in the test build only.
  *
  * @param  frame The window frame to start it on.
  * @param  x Where.
  * @param  y Where.
  *
  * @retval None.
  */
void fgui_test_click( int frame, int x, int y );
#endif

/**
  * @brief  Open the window and run until it is closed.
  *
  * @param  smoke A BMP to save the window into after a few frames, then exit;
  *         NULL for a normal run. CI uses this to see that the GUI starts.
  * @param  tab The page to open on: 0 make, 1 write, 2 read.
  *
  * @retval 0 if it ran, 1 if the window could not be made or the BMP not saved.
  */
int fgui_run( const char *smoke, int tab );

#endif
