Floppy image manipulation
========================= 
ImageFD is a Win32 console application that works with floppy image
files. It is capable of creating and querying image files as well as 
adding/extracting files to/from image files.
ImageFD allows you to create custom floppy images without the need 
for a physical drive. It can handle standard and custom formats.
See the source code for details.

PPK Version 1.x Copyright (c) 2009 Mehdi Sotoodeh. All rights reserved. 

IMPORTANT: You MUST read and accept attached license agreement before 
using this software.


THE SOFTWARE PROVIDED HERE IS FREEWARE AND IS PLACED IN THE PUBLIC DOMAIN 
BY THE AUTHOR WITH THE HOPE THAT IT CAN BE USEFUL.
YOU SHOULD AGREE WITH THE FOLLOWING TERMS AND CONDITIONS BEFORE USING
ANY PART OF THIS PACKAGE.
THIS SOFTWARE AND THE ACCOMPANYING FILES ARE PROVIDED "AS IS" AND WITHOUT 
WARRANTIES AS TO PERFORMANCE OF MERCHANTABILITY OR ANY OTHER WARRANTIES 
WHETHER EXPRESSED OR IMPLIED.  NO WARRANTY OF FITNESS FOR A PARTICULAR 
PURPOSE IS OFFERED.  ADDITIONALLY, THE AUTHOR SHALL NOT BE HELD LIABLE 
FOR ANY LOSS OF DATA, LOSS OF REVENUE, DOWN TIME OR ANY DIRECT OR 
INDIRECT DAMAGE CAUSED BY THIS SOFTWARE. THE USER MUST ASSUME THE ENTIRE 
RISK OF USING THIS SOFTWARE.  


ImageFD - Typical usage:
------------------------
  - Command line help:
    ImageFD.exe
  
  - List of root directory:
    ImageFD.exe BOOT_FD.IMG L

  - List files in a directory:
    ImageFD.exe BOOT_FD.IMG L BIN

  - Create an image file:
    ImageFD.exe my_disk.img F144
    ImageFD.exe my_disk.img A -ar file1.dat data\*.* tools\*.exe

