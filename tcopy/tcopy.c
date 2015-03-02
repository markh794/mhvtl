#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/io.h>
#include <sys/stat.h>
#include </usr/src/linux-2.4.9-13/include/asm-i386/errno.h>
#include <fcntl.h>
#include </usr/include/linux/mtio.h>

char cBuf[600000];

main(argc,argv)
int argc; char **argv;
{
 int res,length;
 int i,idIn,idOut;
 int on = 1,iFirst = 0,iLast = 0;
 int off = 0,iOldByte = 0;
 int eof=0,iFile = 1,iBlock = 0,iF = 0,iError = 0;
 struct mtop mt_cmd;
 mt_cmd.mt_op = MTWEOF;
 mt_cmd.mt_count = 1;


 fprintf(stderr,"*** Written by Odin Explorer programmer\n");

 if(argc == 1)
   {
    fprintf(stderr,"!!! Usage : %s input output \r\n",argv[0]);
    exit(1);
   }
 if(argc == 3)
	 {
	  if((idOut = open(argv[2], 001	)) == -1)
	    {
	      fprintf(stderr,"Can't open output file \r\n");
	      exit(2);
	    }
          mt_cmd.mt_op    = MTSETBLK;
          mt_cmd.mt_count = 0;
          iErr(ioctl(idOut,MTIOCTOP,&mt_cmd));
          mt_cmd.mt_op    = MTSETDRVBUFFER;
          mt_cmd.mt_count = MT_ST_BOOLEANS | !MT_ST_ASYNC_WRITES;
          iErr(ioctl(idOut,MTIOCTOP,&mt_cmd));
	 }
 if((idIn = open(argv[1],000)) == -1)
   {
    fprintf(stderr,"Can't open input file \r\n");
    exit(2);
   }
 mt_cmd.mt_op    = MTSETBLK;
 mt_cmd.mt_count = 0;
 iErr(ioctl(idIn,MTIOCTOP,&mt_cmd));
while(1)
{
   res = read(idIn,&cBuf,600000);
   if(res == -1)
     {
       printf("Read Failed.\n");
       iError++;
       if(iError < 10) { sleep(1); continue; }
       exit(1);
     }
    iError = 0; 
	if(!res)
	  {
		if(eof)
				  break;
		else
			{
			eof = 1;
			if(iFirst == iLast)
			  fprintf(stderr," tcopy: Tape File %d; Record  %d; Size %d \r\n",iFile,iBlock,iOldByte);
			else
           fprintf(stderr," tcopy: Tape File %d; Records: %d to %d; Size: %d\r\n",iFile,iFirst,iLast,iOldByte);
		   fprintf(stderr,"************************************************************\r\n");
	      iFile++;
	      iBlock = 0;
         if( argc == 3 )
            {
             mt_cmd.mt_op    = MTWEOF;
             mt_cmd.mt_count = 1;
             iErr(ioctl(idOut,MTIOCTOP,&mt_cmd));
            }
	      continue;
	      }
	  }
   eof = 0;
   iBlock++;
   length = res;
   if( argc == 3 )
    {
     if ((res = write(idOut,&cBuf,length)) != length)
      {
       printf("Error writing to the file.  res = %d length = %d\n",res,length);
       exit(1);
      }
    }

	if( iBlock == 1 ) { iOldByte = res; iFirst = 1; iLast = 1; continue; }
	if( res == iOldByte )
	  {
	   if(!iF) { iF = 1; iFirst = iBlock-1; }
	   iLast = iBlock;
	   continue;
	  }
   if( iFirst == iLast )
      fprintf(stderr," tcopy: Tape File %d; Record:  %d; Size %d \r\n",iFile,iBlock-1,iOldByte);
   else
    {
     fprintf(stderr," tcopy: Tape File %d; Records: %d to %d; Size: %d\r\n",iFile,iFirst,iLast,iOldByte);
     iFirst = iLast; 
    }  
	iOldByte = res;
   iF = 0;
  }
 fprintf(stderr," tcopy: The end of tape is reached. Files on tape: %d \r\n",iFile-1);
 mt_cmd.mt_op    = MTOFFL;
 mt_cmd.mt_count = 1;
 iErr(ioctl(idIn,MTIOCTOP,&mt_cmd));
 close(idIn);
 if( argc == 3 )
   {
    mt_cmd.mt_op    = MTWEOF;
    mt_cmd.mt_count = 1;
   iErr(ioctl(idOut,MTIOCTOP,&mt_cmd));
    mt_cmd.mt_op    = MTOFFL;
    mt_cmd.mt_count = 1;
   iErr(ioctl(idOut,MTIOCTOP,&mt_cmd));
   close(idOut);
   }
 exit(0);
}

int	iErr(id)
int     id;					/* return code */
{
switch(id)
{
case 5:  /*EIO*****************************************************/
  fprintf(stderr," The requested operation could  not  be  completed.\r\n");
break;
case 28:	/*ENOSPS****************************************************/
  fprintf(stderr," A  write  operation  could  not be completed because the tape reached end-of-medium.\r\n");
break;
case 13:	/*EACCES****************************************************/
  fprintf(stderr," An attempt was made to write or erase a write-protected  tape.\r\n");
break;
case 14:	/*EFAULT****************************************************/
  fprintf(stderr," The command parameters point to memory not belonging to the calling process.\r\n");
break;
case 6:	/*ENXIO****************************************************/
  fprintf(stderr,"During  opening,  the  tape  device does not exist. \r\n");
break;
case 16:	/*EBUSY****************************************************/
  fprintf(stderr,"The device is already in use or  the  driver was unable to allocate a buffer.\r\n");
break;
case 75:	/*EOVERFLOW****************************************************/
  fprintf(stderr,"An attempt was made to read or write a variable-length block that is  larger than  the driver's internal buffer.\r\n");
break;
case 22:	/*EINVAL****************************************************/
  fprintf(stderr,"An ioctl had  an  illegal argument, or a requested block size was illegal.\r\n");
break;
case 38:	/*ENOSYS****************************************************/
  fprintf(stderr,"Unknown ioctl\r\n");
break;
case 30:	/*EROFS****************************************************/
  fprintf(stderr,"Open is attempted with O_WRONLY or O_RDWR when  the  tape  in  the drive is write-protected.\r\n");
break;
}
return(0);			/* set the return status */
}

