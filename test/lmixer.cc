//  lmixer
//  
//  Written by Brandon Zehm
//  caspian@linuxfreak.com
//
//  This software is released under the GNU GPL license.
//  For details see http://www.gnu.org/copyleft/gpl.html
//
//  If you have patches, suggestions, comments, or anything
//  else to tell me, please feel free to email me.
//

#define MIXERVERSION "1.0.7"
#define MIXERDEV    "/dev/mixer"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mixctl.h"


// Initialize functions
void help();
void version();
void scanArgs(int argc, char **argv);
void ShowChannelInfo();
int VerifyChannelName();
int open_mixer();


// Define Variables
char mixdev[256]=MIXERDEV;    // The var with the mixer device
int volume=255;               // Set volume to a value out of range
int channel=255;              // Set channel to a value out of range
char channel_name[32]="BLANK";  // 

// Initialize interface to mixer.h
MixCtl *mixctl;






////////////////////////////////////////////////////////
// Sub:         main()
// Input:       argc and **argv
////////////////////////////////////////////////////////
int main(int argc, char **argv)
{
   // Get command line parameters.
   scanArgs(argc, argv);
   
   // If neither the volume or the channel got set print help.
   if ((volume == 255) && (channel == 255)) {
     help();
     exit(1);
   }
   
   // Check to see if the volume got set
   if (volume == 255) {
     printf ("You must set a volume!\n");
     help();
     exit(1);
   }
   
   // Check to see if the volume got set
   if(strcmp(channel_name, "BLANK")==0) {
     printf ("You must set a mixer channel!\n");
     help();
     exit(1);
   }
   
   
   // If we get here we assume we are doing the defualt operation
   // which is: 
   //            1. open mixer 
   //            2. write volume to a channel
   //            3. quit
   
   
   // Open the mixer or die
   if (open_mixer() != 1) {
     exit(1);
   } 
   
   // Verify that the incoming channel_name is valid
   // and set 'channel' to the corresponding channel number.
   if (VerifyChannelName() == 0) {
     printf("\'%s\' is not a valid channel name!\n", channel_name);
     exit(1);
   }
   
   // Write the volume
   mixctl->setBoth(channel,volume,volume);
   mixctl->writeVol(channel);
   
   
   // Exit
   delete mixctl;
   return 0;
}










////////////////////////////////////////////////////////
// Sub:         scanArgs()
// Input:       argc and *argv
// Output:      Sets initial variables
// Description: Receives incoming data and directs 
//              the flow of the program.
////////////////////////////////////////////////////////
void scanArgs(int argc, char *argv[]){
   for(int i=1;i<argc;i++){
      // Help
      if(strcmp(argv[i], "-h")==0 || strcmp(argv[i], "--help")==0){
        help();
        exit(0);
      }
      // Set Mixer Device
      if(strcmp(argv[i], "-d")==0 || strcmp(argv[i], "--device")==0){
        if(i<argc-1){
          i++;
          sprintf(mixdev, "%s", argv[i]);
        }
        continue;
      }
      // Set new volume
      if(strcmp(argv[i], "-v")==0 || strcmp(argv[i], "--volume")==0){
        if(i<argc-1){
          i++;
          volume = atoi(argv[i]);
        }
        continue;
      }
      // Set new mixer channel
      if(strcmp(argv[i], "-c")==0 || strcmp(argv[i], "--channel")==0){
        if(i<argc-1){
          i++;
          sprintf(channel_name, "%s", argv[i]);
        }
        continue;
      }
      // Show mixer information
      if(strcmp(argv[i], "-i")==0 || strcmp(argv[i], "--info")==0){
        if (open_mixer() != 1) {
          exit(1);
        }
        ShowChannelInfo();
        exit(0);
      }
      // Show version information
      if(strcmp(argv[i], "-V")==0 || strcmp(argv[i], "--version")==0){
        version();
        exit(0);
      }
   }
}






//////////////////////////////////////////////////////////
// Sub:         Help()
// Input:       void
// Output:      
// Description: Prints the help message.
/////////////////////////////////////////////////////////
void help() {
  printf("\n");
  printf("lmixer v%s\n", MIXERVERSION);
  printf("                                                                       \n");
  printf("Usage:   lmixer [options]                                              \n");
  printf("                                                                       \n");
  printf(" -c,--channel   <channel name>   Mixer channel to adjust               \n");
  printf(" -v,--volume    <volume>         Volume (1-100)                        \n");
  printf(" -d,--device    <mixer device>   Use specified mixer device            \n");
  printf(" -i,--info                       Shows the volume of each mixer device \n");
  printf(" -V,--version                    Display version information           \n");
  printf(" -h,--help                       Display this help screen              \n");
  printf("                                                                       \n");
  printf("Typical useage:  'lmixer -c bass -v 85'                                \n");
  printf("\n");
}



//////////////////////////////////////////////////////////
// Sub:         version()
// Input:       void
// Description: Prints version informaion
/////////////////////////////////////////////////////////
void version() {
  printf("lmixer version %s\n", MIXERVERSION);
}
 



////////////////////////////////////////////////////////
// Sub:         open_mixer()
// Input:       int
// Output:      Returns 1 on success 0 on failure.
// Description: Opens the mixer device or dies.
////////////////////////////////////////////////////////
int open_mixer () {
   // Open the mixer, and verify it worked.
   mixctl=new MixCtl(mixdev);
   if(!mixctl->openOK()) {
      fprintf(stdout,"Unable to open mixer device: %s\n", mixdev);
      return(0);
   }
   return(1);
}






////////////////////////////////////////////////////////
// Sub:         ShowChannelInfo()
// Input:       void
// Output:      STDOUT
// Description: Shows a list of channels and their 
//              current volumes.
////////////////////////////////////////////////////////
void ShowChannelInfo () {
  for (int i = 0; i < mixctl->getNrDevices(); i++) {            // For every device,
    if (mixctl->getSupport(i)) {                                // if the device exists,
      volume = mixctl->readVol(i,1);                            // Get the volume
      printf ("%s\t%d\n", mixctl->getName(i), volume);          // and print a line.
    }
  }
}



////////////////////////////////////////////////////////
// Sub:         VerifyChannelName()
// Input:       Reads global var channel_name
// Output:      returns a 1 on success 0 on failure
// Description: Checks to see if 'channel_name' is
//              a valid mixer device, and if it
//              is it sets 'channel' to the number of
//              that device.
////////////////////////////////////////////////////////
int VerifyChannelName () {
  for (int i = 0; i < mixctl->getNrDevices(); i++) {            // For every device,
    if (mixctl->getSupport(i)) {                                // if the device exists,
      if(strcmp(channel_name, mixctl->getName(i))==0) {
        channel = i;
        return 1;
      }
    }
  }
  return 0;
}










