// copyright 2008-2009 paul@ant.sbrk.co.uk. released under GPLv3
// copyright 2009-2009 Wali
// vers 0.6t
#include <stdio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>
#include <math.h>

#include "antdefs.h"

// all version numbering according ant agent for windows 2.2.1
char *releasetime = "Jul 30 2009, 17:42:56";
uint majorrelease = 2;
uint minorrelease = 2;
uint majorbuild = 7;
uint minorbuild = 0;

// it's unclear what the real limits are here-- we should allocate them
// dynamically.
#define MAXLAPS 2048 // max of saving laps data before output with trackpoint data
#define MAXACTIVITIES 2048 // max number of activities
#define MAXTRACKPOINTS 2048 // max trackpoints per track.

double round(double);
short antshort(uchar *data, int off) {
  return data[off] + (data[off+1] * 256);
}
uint antuint(uchar *data, int off) {
  return data[off] + (data[off+1] * 256) + (data[off+2] * 256 * 256) + (data[off+3] * 256 * 256 * 256);
}

typedef struct {
  int activitynum;

  int startlap;
  int stoplap;
  
  uchar sporttype;

} activity_t;

activity_t activitybuf[MAXACTIVITIES];

void decodeactivity(activity_t *pactivity, int activitynum, uchar *data) {
  pactivity->activitynum = activitynum;
  
  pactivity->startlap = antshort(data, 2);
  pactivity->stoplap = antshort(data, 4);

  pactivity->sporttype = data[6];
}

void printactivity(activity_t *pactivity) {
  printf("Activity %d:\n", pactivity->activitynum);
  printf(" Laps %u-%u\n", pactivity->startlap, pactivity->stoplap);
  printf(" Sport type %d\n", pactivity->sporttype);
}


typedef struct {
  int lapnum;

  uint tv_lap; // antuint(lapbuf[lap], 4);
  float tsec; // antuint(lapbuf[lap], 8)
  int cal; // antshort(lapbuf[lap], 36);
  int hr_av; // lapbuf[lap][38];
  int hr_max; // lapbuf[lap][39];   
  int cad; // lapbuf[lap][41];

  int intensity; //lapbuf[lap][40];
  int triggermethod; //lapbuf[lap][42];
  
  float max_speed;
  float dist;
} lap_t;

lap_t lapbuf[MAXLAPS];

void decodelap(lap_t *plap, int lapnum, uchar *data) {
  plap->lapnum = lapnum;
  plap->tv_lap = antuint(data, 4);
  plap->tsec = antuint(data, 8);
  plap->cal = antuint(data, 8);
  plap->cal = antshort(data, 36);
  plap->hr_av = data[38];
  plap->hr_max = data[39];
  plap->cad = data[41];

  plap->intensity = data[40]; // TODO: enum
  plap->triggermethod = data[42]; // TODO: enum

  memcpy((void *)&plap->dist, &data[12], 4);
  memcpy((void *)&plap->max_speed, &data[16], 4);
}

void printlap(lap_t *plap) {
  printf("Lap %d:\n", plap->lapnum);
  printf(" tv_lap: %d\n", plap->tv_lap);
  printf(" tsec: %f\n", plap->tsec);
  printf(" cal: %d\n", plap->cal);
  printf(" hr: %d/%d\n", plap->hr_av, plap->hr_max);
  printf(" cadence: %d\n", plap->cad);

  printf(" intensity: %d\n", plap->intensity);
  printf(" triggermethod: %d\n", plap->triggermethod);

  printf(" dist: %f\n", plap->dist);
  printf(" max_speed: %f\n", plap->max_speed);
}

typedef struct {
  uint tv;
  float alt;
  float dist;
  double lat;
  double lon;
  int hr;
  int cad;
  int u1;
  int u2;
} trackpoint_t;

trackpoint_t trackpointbuf[MAXACTIVITIES][MAXTRACKPOINTS];
int ntrackpoints[MAXACTIVITIES];

void decodetrackpoint(trackpoint_t *ptrackpoint, uchar *data) {
  ptrackpoint->tv = antuint(data, 8);

  memcpy((void *)&ptrackpoint->alt, data+12, 4);
  memcpy((void *)&ptrackpoint->dist, data+16, 4);
  ptrackpoint->lat = antuint(data, 0)*180.0/0x80000000;
  ptrackpoint->lon = antuint(data, 4)*180.0/0x80000000;
  ptrackpoint->hr = data[20];
  ptrackpoint->cad = data[21];
  ptrackpoint->u1 = data[22];
  ptrackpoint->u2 = data[23];
}

void printtrackpoint(trackpoint_t *ptrackpoint) {
  printf("Trackpoint:\n");

  printf(" tv: %d\n", ptrackpoint->tv);
  printf(" alt: %f\n", ptrackpoint->alt);
  printf(" dist: %f\n", ptrackpoint->dist);
  printf(" lat: %f\n", ptrackpoint->lat);
  printf(" lon: %f\n", ptrackpoint->lon);
  printf(" hr: %d\n", ptrackpoint->hr);
  printf(" cad: %d\n", ptrackpoint->cad);
  printf(" u1: %d\n", ptrackpoint->u1);
  printf(" u2: %d\n", ptrackpoint->u2);
}


int gottype;
int sentauth;
int gotwatchid;
int nopairing;
int nowriteauth;
int reset;
int dbg = 0;
int seenphase0 = 0;
int lastphase;
int sentack2;
int newfreq = 0;
int period = 0x1000; // garmin specific broadcast period
int donebind = 0;
int sentgetv;
char *fname = "garmin";

static char ANTSPT_KEY[] = "A8A423B9F55E63C1"; // ANT+Sport key

static uchar ebuf[MESG_DATA_SIZE]; // response event data gets stored here
static uchar cbuf[MESG_DATA_SIZE]; // channel event data gets stored here

int passive;
int semipassive;
int verbose;

int downloadfinished = 0;
int downloadstarted = 0;
int sentid = 0;

uint mydev = 0;
uint peerdev;
uint myid;
uint devid;
uint myauth1;
uint myauth2;
char authdata[32];
uint pairing;
uint isa50;
uint isa405;
uint waitauth;
int nphase0;
char modelname[256];
char devname[256];
ushort part = 0;
ushort ver = 0;
uint unitid = 0;


//char *getversion =	"440dffff00000000fe00000000000000";
//char *getgpsver =		"440dffff0000000006000200ff000000";
char *acks[] = {
  "fe00000000000000", // get version - 255, 248, 253
  "0e02000000000000", // device short name (fr405a) - 525
  //	"1c00020000000000", // no data
  "0a0002000e000000", // unit id - 38
  "0a00020002000000", // send position
  "0a00020005000000", // send time
  "0a000200ad020000", // 4 byte something? 0x10270000 = 10000 dec - 1523
  "0a000200c6010000", // 3 x 4 ints? - 994
  "0a00020035020000", // guessing this is # trackpoints per run - 1066
  "0a00020097000000", // load of software versions - 247
  "0a000200c2010000", // download runs - 27 (#runs), 990?, 12?
  "0a00020075000000", // download laps - 27 (#laps), 149 laps, 12?
  "0a00020006000000", // download trackpoints - 1510/99(run marker), ..1510,12
  "0a000200ac020000",
  ""
};
int sentcmd;

uchar clientid[3][8];

int authfd = -1;
char *authfile;
int outfd; // output file
char *fn = "default_output_file";
char *progname;

#define BSIZE 8*10000
//uchar blast[BSIZE]; // should be like that, but not working
uchar *blast = 0; // first time reading not initialized, but why it is working reading from adr 0?

int blsize = 0;
int bused = 0;
int lseq = -1;

/* round a float as garmin does it! */
/* shoot me for writing this! */
char *
ground(double d)
{
  int neg = 0;
  static char res[30];
  ulong ival;
  ulong l; /* hope it doesn't overflow */

  if (d < 0) {
    neg = 1;
    d = -d;
  }
  ival = floor(d);
  d -= ival;
  l = floor(d*100000000);
  if (l % 10 >= 5)
    l = l/10+1;
  else
    l = l/10;
  sprintf(res,"%s%ld.%07ld", neg?"-":"", ival, l);
  return res;
}

char *
timestamp(void)
{
  struct timeval tv;
  static char time[50];
  struct tm *tmp;

  gettimeofday(&tv, 0);
  tmp = gmtime(&tv.tv_sec);

  sprintf(time, "%02d:%02d:%02d.%02d",
          tmp->tm_hour, tmp->tm_min, tmp->tm_sec, (int)tv.tv_usec/10000);
  return time;
}

uint
randno(void)
{
  uint r;

  int fd = open("/dev/urandom", O_RDONLY);
  if (fd > 0) {
    read(fd, &r, sizeof r);
    close(fd);
  }
  return r;
}

void
print_tcx_header(FILE *tcxfile)
{
  fprintf(tcxfile, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\" ?>\n");
  fprintf(tcxfile, "<TrainingCenterDatabase xmlns=\"http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"http://www.garmin.com/xmlschemas/ActivityExtension/v2 http://www.garmin.com/xmlschemas/ActivityExtensionv2.xsd http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2 http://www.garmin.com/xmlschemas/TrainingCenterDatabasev2.xsd\">\n\n");
  fprintf(tcxfile, "  <Activities>\n");
  return;
}

void
print_tcx_footer(FILE *tcxfile)
{
  fprintf(tcxfile, "        </Track>\n");
  fprintf(tcxfile, "      </Lap>\n");
  fprintf(tcxfile, "      <Creator xsi:type=\"Device_t\">\n");
  fprintf(tcxfile, "        <Name>%s</Name>\n", devname);
  fprintf(tcxfile, "        <UnitId>%u</UnitId>\n", unitid);
  fprintf(tcxfile, "        <ProductID>%u</ProductID>\n", part);
  fprintf(tcxfile, "        <Version>\n");
  fprintf(tcxfile, "          <VersionMajor>%u</VersionMajor>\n", ver/100);
  fprintf(tcxfile, "          <VersionMinor>%u</VersionMinor>\n", ver - ver/100*100);
  fprintf(tcxfile, "          <BuildMajor>0</BuildMajor>\n");
  fprintf(tcxfile, "          <BuildMinor>0</BuildMinor>\n");
  fprintf(tcxfile, "        </Version>\n");
  fprintf(tcxfile, "      </Creator>\n");
  fprintf(tcxfile, "    </Activity>\n");
  fprintf(tcxfile, "  </Activities>\n\n");
  fprintf(tcxfile, "  <Author xsi:type=\"Application_t\">\n");
  fprintf(tcxfile, "    <Name>Garmin ANT Agent(tm)</Name>\n");
  fprintf(tcxfile, "    <Build>\n");
  fprintf(tcxfile, "      <Version>\n");
  fprintf(tcxfile, "        <VersionMajor>%u</VersionMajor>\n", majorrelease);
  fprintf(tcxfile, "        <VersionMinor>%u</VersionMinor>\n", minorrelease);
  fprintf(tcxfile, "        <BuildMajor>%u</BuildMajor>\n", majorbuild);
  fprintf(tcxfile, "        <BuildMinor>%u</BuildMinor>\n", minorbuild);
  fprintf(tcxfile, "      </Version>\n");
  fprintf(tcxfile, "      <Type>Release</Type>\n");
  fprintf(tcxfile, "      <Time>%s</Time>\n", releasetime);
  fprintf(tcxfile, "      <Builder>sqa</Builder>\n");
  fprintf(tcxfile, "    </Build>\n");
  fprintf(tcxfile, "    <LangID>EN</LangID>\n");
  fprintf(tcxfile, "    <PartNumber>006-A0214-00</PartNumber>\n");
  fprintf(tcxfile, "  </Author>\n\n");
  fprintf(tcxfile, "</TrainingCenterDatabase>\n");
  return;
}


#pragma pack(1)
struct ack_msg {
  uchar code;
  uchar atype;
  uchar c1;
  uchar c2;
  uint id;
};

struct auth_msg {
  uchar code;
  uchar atype;
  uchar phase;
  uchar u1;
  uint id;
  uint auth1;
  uint auth2;
  uint fill1;
  uint fill2;
};

struct pair_msg {
  uchar code;
  uchar atype;
  uchar phase;
  uchar u1;
  uint id;
  char devname[16];
};

#pragma pack()
#define ACKSIZE 8 // above structure must be this size
#define AUTHSIZE 24 // ditto
#define PAIRSIZE 16

decode(ushort bloblen, ushort pkttype, ushort pktlen, int dsize, uchar *data)
{
  int i;
  int j;
  float tsec;
  int doff = 20;
  char model[256];
  char gpsver[256];
  uint tv;
  uint tv_previous = 0;
  time_t ttv;
  char tbuf[100];
  struct tm *tmp;
  uint nruns;
  uint tv_lap;
  static ushort lap = 0;
  static ushort lastlap = 0;
  static ushort activity = 0;
  static short previousactivity_id = -1;
  static short activity_id = -1;
  static FILE *tcxfile = NULL;
  static ushort track_pause = 0;


  printf("decode %d %d %d %d\n", bloblen, pkttype, pktlen, dsize);
  switch (pkttype) {
  case 255:
    memset(model, 0, sizeof model);
    memcpy(model, data+doff+4, dsize-4);
    part=antshort(data, doff);
    ver=antshort(data, doff+2);
    printf("%d Part#: %d ver: %d Name: %s\n", pkttype,
           part, ver, model);
    break;
  case 248:
    memset(gpsver, 0, sizeof gpsver);
    memcpy(gpsver, data+doff, dsize);
    printf("%d GPSver: %s\n", pkttype,
           gpsver);
    break;
  case 253:
    printf("%d Unknown\n", pkttype);
    for (i = 0; i < pktlen; i += 3)
      printf("%d.%d.%d\n", data[doff+i], data[doff+i+1], data[doff+i+2]);
    break;
  case 525:
    memset(devname, 0, sizeof devname);
    memcpy(devname, data+doff, dsize);
    printf("%d Devname %s\n", pkttype, devname);
    break;
  case 12:
    printf("%d xfer complete", pkttype);
    for (i = 0; i < pktlen; i += 2)
      printf(" %u", antshort(data, doff+i));
    printf("\n");
    switch (antshort(data, doff)) {
    case 6:
      // last file completed, add footer and close file
      print_tcx_footer(tcxfile);
      fclose(tcxfile);
      break;
    case 117:
      break;
    case 450:
      break;
    default:
      break;
    }
    break;
  case 38:
    unitid = antuint(data, doff);
    printf("%d unitid %u\n", pkttype, unitid);
    break;
  case 27:
    nruns = data[doff] + data[doff+1] * 256;
    printf("%d nruns %u\n", pkttype, nruns);
    break;
  case 1523:
  case 994:
  case 1066:
    printf("%d ints?", pkttype);
    for (i = 0; i < pktlen; i += 4)
      printf(" %u", antuint(data, doff+i));
    printf("\n");
    break;
  case 14:
    printf("%d time: ", pkttype);
    printf("%02u-%02u-%u %02u:%02u:%02u\n", data[doff], data[doff+1], antshort(data, doff+2), data[doff+4], data[doff+6], data[doff+7]);
    break;
  case 17:
    printf("%d position ? ", pkttype);
    for (i = 0; i < pktlen; i += 4)
      printf(" %u", antuint(data, doff+i));
    printf("\n");
    break;
  case 99:
    printf("%d trackindex %u\n", pkttype, antshort(data, doff));
    printf("%d shorts?", pkttype);
    for (i = 0; i < pktlen; i += 2)
      printf(" %u", antshort(data, doff+i));
    printf("\n");
    activity_id = antshort(data, doff);
    break;
  case 990:
    printf("%d activity %u lap %u-%u sport %u\n", pkttype,
           antshort(data, doff), antshort(data, doff+2),
           antshort(data, doff+4), data[doff+6]);
    printf("%d shorts?", pkttype);
    for (i = 0; i < pktlen; i += 2)
      printf(" %u", antshort(data, doff+i));
    printf("\n");
    activity = antshort(data, doff);
    if (activity < MAXACTIVITIES) {
      decodeactivity(&activitybuf[activity], activity, &data[doff]);
      printactivity(&activitybuf[activity]);
    } else {
      printf("Error: activity and lap data temporary array out of range %u!\n", activity);
      exit(0);
    }
    break;
  case 1510:
    printf("%d trackpoints", pkttype);
    for (i = 0; i < 4 && i < pktlen; i += 4)
      printf(" %u", antuint(data, doff+i));
    printf("\n");
    // if trackpoints are split into more than one message 1510, do not add xml head again
    if (previousactivity_id != activity_id) {
      // close previous file if it is not the first activity to be downloaded
      if (previousactivity_id > -1) {
        // add xml footer and close file, the next file will be open further down
        print_tcx_footer(tcxfile);
        fclose(tcxfile);
      }
      // use first lap starttime as filename
      lap = activitybuf[activity_id].startlap;
      if (dbg) printf("lap %u activity_id %u\n", lap, activity_id);
      tv_lap = lapbuf[lap].tv_lap; 
      ttv = tv_lap + 631065600; // garmin epoch offset
      strftime(tbuf, sizeof tbuf, "%Y-%m-%d-%H%M%S.TCX", localtime(&ttv));
      // open file and start with header of xml file
      tcxfile = fopen(tbuf, "wt");
      print_tcx_header(tcxfile);
    }
    for (i = 4; i < pktlen; i += 24) {
      if (ntrackpoints[activity_id] >= MAXTRACKPOINTS) {
        printf("Not enough trackpoints.\n");
        exit(1);
      }
      
      trackpoint_t *ptrackpoint = 
        &trackpointbuf[activity_id][ntrackpoints[activity_id]];
      decodetrackpoint(ptrackpoint, &data[doff+i]);
      ntrackpoints[activity_id]++;
      printtrackpoint(ptrackpoint);

      tv = ptrackpoint->tv;
      tv_lap = lapbuf[lap].tv_lap; 
      if (((tv > tv_lap) || (tv == tv_lap && lap == activitybuf[activity_id].startlap)) && lap <= lastlap) {
        ttv = tv_lap + 631065600; // garmin epoch offset
        strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%SZ", gmtime(&ttv));
        tsec = lapbuf[lap].tsec;
        if (lap == activitybuf[activity_id].startlap) {
          fprintf(tcxfile, "    <Activity Sport=\"");
          switch(activitybuf[activity_id].sporttype) {
          case 0: fprintf(tcxfile, "Running"); break;
          case 1: fprintf(tcxfile, "Biking"); break;
          case 2: fprintf(tcxfile, "Other"); break;
          default: fprintf(tcxfile, "unknown value: %d",activitybuf[activity_id].sporttype);
          }
          fprintf(tcxfile, "\">\n");
          fprintf(tcxfile, "      <Id>%s</Id>\n", tbuf);
        } else {
          fprintf(tcxfile, "        </Track>\n");
          fprintf(tcxfile, "      </Lap>\n");
        }
        fprintf(tcxfile, "      <Lap StartTime=\"%s\">\n", tbuf);
        fprintf(tcxfile, "        <TotalTimeSeconds>%s</TotalTimeSeconds>\n", ground(tsec/100));
        fprintf(tcxfile, "        <DistanceMeters>%s</DistanceMeters>\n", ground(lapbuf[lap].dist));
        fprintf(tcxfile, "        <MaximumSpeed>%s</MaximumSpeed>\n", ground(lapbuf[lap].max_speed));
        fprintf(tcxfile, "        <Calories>%d</Calories>\n", lapbuf[lap].cal);
        if (lapbuf[lap].hr_av > 0) {
          fprintf(tcxfile, "        <AverageHeartRateBpm xsi:type=\"HeartRateInBeatsPerMinute_t\">\n");
          fprintf(tcxfile, "          <Value>%d</Value>\n", lapbuf[lap].hr_av);
          fprintf(tcxfile, "        </AverageHeartRateBpm>\n");
        }
        if (lapbuf[lap].hr_max > 0) {
          fprintf(tcxfile, "        <MaximumHeartRateBpm xsi:type=\"HeartRateInBeatsPerMinute_t\">\n");
          fprintf(tcxfile, "          <Value>%d</Value>\n", lapbuf[lap].hr_max);
          fprintf(tcxfile, "        </MaximumHeartRateBpm>\n");
        }
        fprintf(tcxfile, "        <Intensity>");
        switch (lapbuf[lap].intensity) {
        case 0: fprintf(tcxfile, "Active"); break;
        case 1: fprintf(tcxfile, "Rest"); break;
        default: fprintf(tcxfile, "unknown value: %d", lapbuf[lap].intensity);
        }
        fprintf(tcxfile, "</Intensity>\n");
        // for bike the average cadence of this lap is here
        if (activitybuf[activity_id].sporttype == 1) {
          if (lapbuf[lap].cad != 255) {
            fprintf(tcxfile, "        <Cadence>%d</Cadence>\n", lapbuf[lap].cad);
          }
        }
        fprintf(tcxfile, "        <TriggerMethod>");
        switch(lapbuf[lap].triggermethod) {
        case 4: fprintf(tcxfile, "Heartrate"); break;
        case 3: fprintf(tcxfile, "Time"); break;
        case 2: fprintf(tcxfile, "Location"); break;
        case 1: fprintf(tcxfile, "Distance"); break;
        case 0: fprintf(tcxfile, "Manual"); break;
        default: fprintf(tcxfile, "unknown value: %d", lapbuf[lap].triggermethod);
        }
        fprintf(tcxfile, "</TriggerMethod>\n");
        // I prefere the average run cadence here than at the end of this lap according windows ANTagent
        if (activitybuf[activity_id].sporttype == 0) {
          if (lapbuf[lap].cad != 255) {
            fprintf(tcxfile, "        <Extensions>\n");
            fprintf(tcxfile, "          <LX xmlns=\"http://www.garmin.com/xmlschemas/ActivityExtension/v2\">\n");
            fprintf(tcxfile, "            <AvgRunCadence>%d</AvgRunCadence>\n", lapbuf[lap].cad);
            fprintf(tcxfile, "          </LX>\n");
            fprintf(tcxfile, "        </Extensions>\n");
          }
        }
        fprintf(tcxfile, "        <Track>\n");
        lap++;
        // if the previous trackpoint has same second as lap time display the trackpoint again
        if (dbg) printf("i %u tv %d tv_lap %d tv_previous %d\n", i, tv, tv_lap, tv_previous);
        if (tv_previous == tv_lap) {
          i -= 24;
          tv = tv_previous;
        }
        track_pause = 0;
      } // end of if (tv >= tv_lap && lap <= lastlap)
      ttv = tv+631065600; // garmin epoch offset
      tmp = gmtime(&ttv);
      strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%SZ", tmp);  // format for printing

      // track pause only if following trackpoint is aswell 'timemarker' with utopic distance
      if (track_pause && ptrackpoint->dist > (float)40000000) {
        fprintf(tcxfile, "        </Track>\n");
        fprintf(tcxfile, "        <Track>\n");
      }
      fprintf(tcxfile, "          <Trackpoint>\n");
      fprintf(tcxfile, "            <Time>%s</Time>\n",tbuf);
      if (ptrackpoint->lat < 90) {
        fprintf(tcxfile, "            <Position>\n");
        fprintf(tcxfile, "              <LatitudeDegrees>%s</LatitudeDegrees>\n",
                ground(ptrackpoint->lat));
        fprintf(tcxfile, "              <LongitudeDegrees>%s</LongitudeDegrees>\n",
                ground(ptrackpoint->lon));
        fprintf(tcxfile, "            </Position>\n");
        fprintf(tcxfile, "            <AltitudeMeters>%s</AltitudeMeters>\n", ground(ptrackpoint->alt));
      }
      // last trackpoint has utopic distance, 40000km should be enough, hack?
      if (ptrackpoint->dist < (float)40000000) {
        fprintf(tcxfile, "            <DistanceMeters>%s</DistanceMeters>\n", ground(ptrackpoint->dist));
      }
      if (ptrackpoint->hr > 0) {
        fprintf(tcxfile, "            <HeartRateBpm xsi:type=\"HeartRateInBeatsPerMinute_t\">\n");
        fprintf(tcxfile, "              <Value>%d</Value>\n", ptrackpoint->hr);
        fprintf(tcxfile, "            </HeartRateBpm>\n");
      }
      // for bikes the cadence is written here and for the footpod in <Extensions>, why garmin?
      if (activitybuf[activity_id].sporttype == 1) {
        if (lapbuf[lap].cad != 255) {
          fprintf(tcxfile, "            <Cadence>%d</Cadence>\n", lapbuf[lap].cad);
        }
      }
      if (ptrackpoint->dist < (float)40000000) {
        fprintf(tcxfile, "            <SensorState>%s</SensorState>\n", ptrackpoint->u1 ? "Present" : "Absent");
        if (ptrackpoint->u1 == 1 || ptrackpoint->cad != 255) {
          fprintf(tcxfile, "            <Extensions>\n");
          fprintf(tcxfile, "              <TPX xmlns=\"http://www.garmin.com/xmlschemas/ActivityExtension/v2\" CadenceSensor=\"");
          // get type of pod from data, could not figure it out, so using sporttyp of first track
          if (activitybuf[activity_id].sporttype == 1) {
            fprintf(tcxfile, "Bike\"/>\n");
          } else {
            fprintf(tcxfile, "Footpod\"");
            if (ptrackpoint->cad != 255) {
              fprintf(tcxfile, ">\n");
              fprintf(tcxfile, "                <RunCadence>%d</RunCadence>\n", ptrackpoint->cad);
              fprintf(tcxfile, "              </TPX>\n");
            } else {
              fprintf(tcxfile, "/>\n");
            }
          }
          fprintf(tcxfile, "            </Extensions>\n");
        }
        track_pause = 0;
      }
      fprintf(tcxfile, "          </Trackpoint>\n");
      // maybe if we recieve utopic position and distance this tells pause in the run (stop and go) if not begin or end of lap
      if (ptrackpoint->dist > (float)40000000 && track_pause == 0) {
        track_pause = 1;
        if (dbg) printf("track pause (stop and go)\n");
      } else {
        track_pause = 0;
      }
      tv_previous = tv;
    } // end of for (i = 4; i < pktlen; i += 24)
    previousactivity_id = activity_id;
    break;
  case 149:
    printf("%d Lap data id: %u %u\n", pkttype,
           antshort(data, doff), antshort(data, doff+2));
    lap = antshort(data, doff);
    if (lap < MAXLAPS) {
      decodelap(&lapbuf[lap], lap, data+doff);
      printlap(&lapbuf[lap]);
      lap++;
      lastlap = lap;
    }
    else {
      printf("Not enough laps.");
      exit(1);
    }
    break;
  case 247:
    memset(modelname, 0, sizeof modelname);
    memcpy(modelname, data+doff+88, dsize-88);
    printf("%d Device name %s\n", pkttype, modelname);
    break;
  default:
    printf("don't know how to decode packet type %d\n", pkttype);
    for (i = doff; i < dsize && i < doff+pktlen; i++)
      printf("%02x", data[i]);
    printf("\n");
    for (i = doff; i < dsize && i < doff+pktlen; i++)
      if (isprint(data[i]))
        printf("%c", data[i]);
      else
        printf(".");
    printf("\n");
  }
}

void
usage(void)
{
  fprintf(stderr, "Usage: %s -a authfile\n"
          "[ -o outfile ]\n"
          "[ -d devno ]\n"
          "[ -i id ]\n"
          "[ -m mydev ]\n"
          "[ -p ]\n",
          progname
          );
  exit(1);
}

uchar
chevent(uchar chan, uchar event)
{
  uchar seq;
  uchar last;
  uchar status;
  uchar phase;
  uint newdata;
  struct ack_msg ack;
  struct auth_msg auth;
  struct pair_msg pair;
  uint id;
  int i;
  uint cid;
  if (dbg) printf("chevent %02x %02x\n", chan, event);

  if (event == EVENT_RX_BROADCAST) {
    status = cbuf[1] & 0xd7;
    newdata = cbuf[1] & 0x20;
    phase = cbuf[2];
  }
  cid = antuint(cbuf, 4);
  memcpy((void *)&id, cbuf+4, 4);

  if (dbg)
    fprintf(stderr, "cid %08x myid %08x\n", cid, myid);
  if (dbg && event != EVENT_RX_BURST_PACKET) {
    fprintf(stderr, "chan %d event %02x channel open: ", chan, event);
    for (i = 0; i < 8; i++)
      fprintf(stderr, "%02x", cbuf[i]);
    fprintf(stderr, "\n");
  }
	
  switch (event) {
  case EVENT_RX_BROADCAST:
    lastphase = phase; // store the last phase we see the watch broadcast
    if (dbg) printf("lastphase %d\n", lastphase);
    if (!pairing && !nopairing)
      pairing = cbuf[1] & 8;
    if (!gottype) {
      gottype = 1;
      isa50 = cbuf[1] & 4;
      isa405 = cbuf[1] & 1;
      if ((isa50 && isa405) || (!isa50 && !isa405)) {
        fprintf(stderr, "50 %d and 405 %d\n", isa50, isa405);
        exit(1);
      }
    }
    if (verbose) {
      switch (phase) {
      case 0:
        fprintf(stderr, "%s BC0 %02x %d %d %d PID %d %d %d %c%c\n",
                timestamp(),
                cbuf[0], cbuf[1] & 0xd7, cbuf[2], cbuf[3],
                antshort(cbuf, 4), cbuf[6], cbuf[7],
                (cbuf[1] & 0x20) ? 'N' : ' ', (cbuf[1] & 0x08) ? 'P' : ' '
                );
        break;
      case 1:
        fprintf(stderr, "%s BC1 %02x %d %d %d CID %08x %c%c\n",
                timestamp(),
                cbuf[0], cbuf[1] & 0xd7, cbuf[2], cbuf[3], cid,
                (cbuf[1] & 0x20) ? 'N' : ' ', (cbuf[1] & 0x08) ? 'P' : ' '
                );
        break;
        fprintf(stderr, "%s BCX %02x %d %d %d PID %d %d %d %c%c\n",
                timestamp(),
                cbuf[0], cbuf[1] & 0xd7, cbuf[2], cbuf[3],
                antshort(cbuf, 4), cbuf[6], cbuf[7],
                (cbuf[1] & 0x20) ? 'N' : ' ', (cbuf[1] & 0x08) ? 'P' : ' '
                );
      default:
        break;
      }
    }

    if (dbg)
      printf("watch status %02x stage %d id %08x\n", status, phase, id);

    if (!sentid) {
      sentid = 1;
      ANT_RequestMessage(chan, MESG_CHANNEL_ID_ID); /* request sender id */
    }

    // if we don't see a phase 0 message first, reset the watch
    if (reset || (phase != 0 && !seenphase0)) {
      fprintf(stderr, "resetting\n");
      ack.code = 0x44; ack.atype = 3; ack.c1 = 0x00; ack.c2 = 0x00; ack.id = 0;
      ANT_SendAcknowledgedData(chan, (void *)&ack); // tell garmin we're finished
      sleep(1);
      exit(1);
    }
    switch (phase) {
    case 0:
      seenphase0 = 1;
      nphase0++;
      if (nphase0 % 10 == 0)
        donebind = 0;
      if (newfreq) {
        // switch to new frequency
        ANT_SetChannelPeriod(chan, period);
        ANT_SetChannelSearchTimeout(chan, 3);
        ANT_SetChannelRFFreq(chan, newfreq);
        newfreq = 0;
      }
      // phase 0 seen after reset at end of download
      if (downloadfinished) {
        fprintf(stderr, "finished\n");
        exit(0);
      }
      // generate a random id if pairing and user didn't specify one
      if (pairing && !myid) {
        myid = randno();
        fprintf(stderr, "pairing, using id %08x\n", myid);
      }
      // need id codes from auth file if not pairing
      // TODO: handle multiple watches
      // BUG: myauth1 should be allowed to be 0
      if (!pairing && !myauth1) {
        int nr;
        printf("reading auth data from %s\n", authfile);
        authfd = open(authfile, O_RDONLY);
        if (authfd < 0) {
          perror(authfile);
          fprintf(stderr, "No auth data. Need to pair first\n");
          exit(1);
        }
        nr = read(authfd, authdata, 32);
        close(authfd);
        if (nr != 32 && nr != 24) {
          fprintf(stderr, "bad auth file len %d != 32 or 24\n", nr);
          exit(1);
        }
        // BUG: auth file not portable
        memcpy((void *)&myauth1, authdata+16, 4);
        memcpy((void *)&myauth2, authdata+20, 4);
        memcpy((void *)&mydev, authdata+12, 4);
        memcpy((void *)&myid, authdata+4, 4);
        if (dbg)
          fprintf(stderr, "dev %08x auth %08x %08x id %08x\n",
                  mydev, myauth1, myauth2, myid);
      }
      // bind to watch
      if (!donebind && devid) {
        donebind = 1;
        if (isa405)
          newfreq = 0x32;
        ack.code = 0x44; ack.atype = 2; ack.c1 = isa50 ? 0x32 : newfreq; ack.c2 = 0x04;
        ack.id = myid;
        ANT_SendAcknowledgedData(chan, (void *)&ack); // bind
      } else {
        if (dbg) printf("donebind %d devid %x\n", donebind, devid);
      }
      break;
    case 1:
      if (dbg) printf("case 1 %x\n", peerdev);
      if (peerdev) {
        if (dbg) printf("case 1 peerdev\n");
        // if watch has sent id
        if (mydev != 0 && peerdev != mydev) {
          fprintf(stderr, "Don't know this device %08x != %08x\n", peerdev, mydev);
        } else if (!sentauth && !waitauth) {
          if (dbg) printf("case 1 diffdev\n");
          assert(sizeof auth == AUTHSIZE);
          auth.code = 0x44; auth.atype = 4; auth.phase = 3; auth.u1 = 8;
          auth.id = myid; auth.auth1 = myauth1; auth.auth2 = myauth2;
          auth.fill1 = auth.fill2 = 0;
          sentauth = 1;
          ANT_SendBurstTransfer(chan, (void *)&auth, (sizeof auth)/8); // send our auth data
        }
      }
      if (dbg) printf("case 1 cid %x myid %x\n", cid, myid);
      if (!sentack2 && cid == myid && !waitauth) {
        sentack2 = 1;
        if (dbg) printf("sending ack2\n");
        // if it did bind to me before someone else
        ack.code = 0x44; ack.atype = 4; ack.c1 = 0x01; ack.c2 = 0x00;
        ack.id = myid;
        ANT_SendAcknowledgedData(chan, (void *)&ack); // request id
      }
      break;
    case 2:
      // successfully authenticated
      if (!downloadstarted) {
        downloadstarted = 1;
        if (dbg) printf("starting download\n");
        ack.code = 0x44; ack.atype = 6; ack.c1 = 0x01; ack.c2 = 0x00; ack.id = 0;
        //ANT_SendAcknowledgedData(chan, (void *)&ack); // tell garmin to start upload
      }
      if (downloadfinished) {
        if (dbg) printf("finished download\n");
        ack.code = 0x44; ack.atype = 3; ack.c1 = 0x00; ack.c2 = 0x00; ack.id = 0;
        if (!passive) ANT_SendAcknowledgedData(chan, (void *)&ack); // tell garmin we're finished
      }
      break;
    case 3:
      if (pairing) {
        // waiting for the user to pair
        printf("Please press \"View\" on watch to confirm pairing\n");
        waitauth = 2; // next burst data is auth data
      } else {
        if (dbg) printf("not sure why in phase 3\n");
        if (!sentgetv) {
          sentgetv = 1;
          //ANT_SendBurstTransferA(chan, getversion, strlen(getversion)/16);
        }
      }
      break;
    default:
      if (dbg) fprintf(stderr, "Unknown phase %d\n", phase);
      break;
    }
    break;
  case EVENT_RX_BURST_PACKET:
    // now handled in coalesced burst below
    if (dbg) printf("burst\n");
    break;
  case EVENT_RX_FAKE_BURST:
    if (dbg) printf("rxfake burst pairing %d blast %ld waitauth %d\n",
                    pairing, (long)blast, waitauth);
    blsize = *(int *)(cbuf+4);
    memcpy(&blast, cbuf+8, 4);
    if (dbg) {
      printf("fake burst %d %lx ", blsize, (long)blast);
      for (i = 0; i < blsize && i < 64; i++)
        printf("%02x", blast[i]);
      printf("\n");
      for (i = 0; i < blsize; i++)
        if (isprint(blast[i]))
          printf("%c", blast[i]);
        else
          printf(".");
      printf("\n");
    }
    if (sentauth) {
      static int nacksent = 0;
      char *ackdata;
      static uchar ackpkt[100];
      // ack the last packet
      ushort bloblen = blast[14]+256*blast[15];
      ushort pkttype = blast[16]+256*blast[17];
      ushort pktlen = blast[18]+256*blast[19];
      if (bloblen == 0) {
        if (dbg) printf("bloblen %d, get next data\n", bloblen);
        // request next set of data
        ackdata = acks[nacksent++];
        if (!strcmp(ackdata, "")) { // finished
          printf("acks finished, resetting\n");
          ack.code = 0x44; ack.atype = 3; ack.c1 = 0x00;
          ack.c2 = 0x00; ack.id = 0;
          ANT_SendAcknowledgedData(chan, (void *)&ack); // go to idle
          sleep(1);
          exit(1);
        }
        if (dbg) printf("got type 0, sending ack %s\n", ackdata);
        sprintf(ackpkt, "440dffff00000000%s", ackdata);
      } else if (bloblen == 65535) {
        // repeat last ack
        if (dbg) printf("repeating ack %s\n", ackpkt);
        ANT_SendBurstTransferA(chan, ackpkt, strlen(ackpkt)/16);
      } else {
        if (dbg) printf("non-0 bloblen %d\n", bloblen);
        decode(bloblen, pkttype, pktlen, blsize, blast);
        sprintf(ackpkt, "440dffff0000000006000200%02x%02x0000", pkttype%256, pkttype/256);
      }
      if (dbg) printf("received pkttype %d len %d\n", pkttype, pktlen);
      if (dbg) printf("acking %s\n", ackpkt);
      ANT_SendBurstTransferA(chan, ackpkt, strlen(ackpkt)/16);
    } else if (!nopairing && pairing && blast) {
      memcpy(&peerdev, blast+12, 4);
      if (dbg)
        printf("watch id %08x waitauth %d\n", peerdev, waitauth);
      if (mydev != 0 && peerdev != mydev) {
        fprintf(stderr, "Don't know this device %08x != %08x\n", peerdev, mydev);
        exit(1);
      }
      if (waitauth == 2) {
        int nw;
        // should be receiving auth data
        if (nowriteauth) {
          printf("Not overwriting auth data\n");
          exit(1);
        }
        printf("storing auth data in %s\n", authfile);
        authfd = open(authfile, O_WRONLY|O_CREAT, 0644);
        if (authfd < 0) {
          perror(authfile);
          exit(1);
        }
        nw = write(authfd, blast, blsize);
        if (nw != blsize) {
          fprintf(stderr, "auth write failed fd %d %d\n", authfd, nw);
          perror("write");
          exit(1);
        }
        close(authfd);
        //exit(1);
        pairing = 0;
        waitauth = 0;
        reset = 1;
      }
      if (pairing && !waitauth) {
        //assert(sizeof pair == PAIRSIZE);
        pair.code = 0x44; pair.atype = 4; pair.phase = 2; pair.id = myid;
        bzero(pair.devname, sizeof pair.devname);
        //if (peerdev <= 9999999) // only allow 7 digits
        //sprintf(pair.devname, "%u", peerdev);
        strcpy(pair.devname, fname);
        //else
        //	fprintf(stderr, "pair dev name too large %08x \"%d\"\n", peerdev, peerdev);
        pair.u1 = strlen(pair.devname);
        printf("sending pair data for dev %s\n", pair.devname);
        waitauth = 1;
        if (isa405 && pairing) {
          // go straight to storing auth data
          waitauth = 2;
        }
        ANT_SendBurstTransfer(chan, (void *)&pair, (sizeof pair)/8) ; // send pair data
      } else {
        if (dbg) printf("not pairing\n");
      }
    } else if (!gotwatchid && (lastphase == 1)) {
      static int once = 0;
      gotwatchid = 1;
      // garmin sending authentication/identification data
      if (!once) {
        int i;
        once = 1;
        if (dbg)
          fprintf(stderr, "id data: ");
      }
      if (dbg)
        for (i = 0; i < blsize; i++)
          fprintf(stderr, "%02x", blast[i]);
      if (dbg)
        fprintf(stderr, "\n");
      memcpy(&peerdev, blast+12, 4);
      if (dbg)
        printf("watch id %08x\n", peerdev);
      if (mydev != 0 && peerdev != mydev) {
        fprintf(stderr, "Don't know this device %08x != %08x\n", peerdev, mydev);
        exit(1);
      }
    } else if (lastphase == 2) {
      int nw;
      static int once = 0;
      printf("once %d\n", once);
      // garmin uploading in response to sendack3
      // in this state we're receiving the workout data
      if (!once) {
        printf("receiving\n");
        once = 1;
        outfd = open(fn, O_WRONLY|O_CREAT, 0644);
        if (outfd < 0) {
          perror(fn);
          exit(1);
        }
      }
      if (last) {
        nw = write(outfd, blast, blsize);
        if (nw != blsize) {
          fprintf(stderr, "data write failed fd %d %d\n", outfd, nw);
          perror("write");
          exit(1);
        }
        close(outfd);
        downloadfinished = 1;
      }
    } else if (0 && last) {
      if (dbg) {
        fprintf(stderr, "auth response: ");
        for (i = 0; i < blsize; i++)
          fprintf(stderr, "%02x", cbuf[i]);
        fprintf(stderr, "\n");
      }
      if (blast[10] == 2) {
        fprintf(stderr, "authentication failed\n");
        exit(1);
      }
    } else if (last) {
      fprintf(stderr, "data in state xx: ");
      int i;
      for (i = 0; i < blsize; i++)
        fprintf(stderr, "%02x", blast[i]);
      fprintf(stderr, "\n");
      sentcmd = 1000;
      switch (sentcmd) {
      case 1000:
        break;
      case 0:
        sentcmd++;
        //ANT_SendBurstTransferA(chan, getgpsver, strlen(getgpsver)/16);
        break;
      case 999:
        printf("finished\n");
        exit(1);
      default:
        sleep(1);
        sentcmd = 1;
        //printf("sending command %d %s\n", sentcmd-1, cmds[sentcmd-1]);
        //ANT_SendBurstTransferA(chan, cmds[sentcmd-1],
        //	strlen(cmds[sentcmd-1])/16);
        sentcmd++;
        //if(!strcmp(cmds[sentcmd-1], "END"))
        //	sentcmd = 999;
        break;
      }
    }
    if (dbg) printf("continuing after burst\n");
    break;
  }
  return 1;
}

uchar
revent(uchar chan, uchar event)
{
  struct ack_msg ack;
  int i;

  if (dbg) printf("revent %02x %02x\n", chan, event);
  switch (event) {
  case EVENT_TRANSFER_TX_COMPLETED:
    if (dbg) printf("Transfer complete %02x\n", ebuf[1]);
    break;
  case INVALID_MESSAGE:
    printf("Invalid message %02x\n", ebuf[1]);
    break;
  case RESPONSE_NO_ERROR:
    switch (ebuf[1]) {
    case MESG_ASSIGN_CHANNEL_ID:
      ANT_AssignChannelEventFunction(chan, chevent, cbuf);
      break;
    case MESG_OPEN_CHANNEL_ID:
      printf("channel open, waiting for broadcast\n");
      break;
    default:
      if (dbg) printf("Message %02x NO_ERROR\n", ebuf[1]);
      break;
    }
    break;
  case MESG_CHANNEL_ID_ID:
    devid = antshort(ebuf, 1);
    if (mydev == 0 || devid == mydev%65536) {
      if (dbg)
        printf("devid %08x myid %08x\n", devid, myid);
    } else {
      printf("Ignoring unknown device %08x, mydev %08x\n", devid, mydev);
      devid = sentid = 0; // reset
    }
    break;
  case MESG_NETWORK_KEY_ID:
  case MESG_SEARCH_WAVEFORM_ID:
  case MESG_OPEN_CHANNEL_ID:
    printf("response event %02x code %02x\n", event, ebuf[2]);
    for (i = 0; i < 8; i++)
      fprintf(stderr, "%02x", ebuf[i]);
    fprintf(stderr, "\n");
    break;
  case MESG_CAPABILITIES_ID:
    if (dbg)
      printf("capabilities chans %d nets %d opt %02x adv %02x\n",
             ebuf[0], ebuf[1], ebuf[2], ebuf[3]);
    break;
  case MESG_CHANNEL_STATUS_ID:
    if (dbg)
      printf("channel status %d\n", ebuf[1]);
    break;
  case EVENT_RX_FAIL:
    // ignore this
    break;
  default:
    printf("Unhandled response event %02x\n", event);
    break;
  }
  return 1;
}

main(int ac, char *av[])
{
  int devnum = 0;
  int chan = 0;
  int net = 0;
  int chtype = 0; // wildcard
  int devno = 0; // wildcard
  int devtype = 0; // wildcard
  int manid = 0; // wildcard
  int freq = 0x32; // garmin specific radio frequency
  int srchto = 255; // max timeout
  int waveform = 0x0053; // aids search somehow
  int c;
  extern char *optarg;
  extern int optind, opterr, optopt;

  // default auth file //
  if (getenv("HOME")) {
    authfile = malloc(strlen(getenv("HOME"))+strlen("/.gant")+1);
    if (authfile)
      sprintf(authfile, "%s/.gant", getenv("HOME"));
  }
  progname = av[0];
  while ((c = getopt(ac, av, "a:o:d:i:m:PpvDrnzf:")) != -1) {
    switch(c) {
    case 'a':
      authfile = optarg;
      break;
    case 'f':
      fname = optarg;
      break;
    case 'o':
      fn = optarg;
      break;
    case 'd':
      devnum = atoi(optarg);
      break;
    case 'i':
      myid = atoi(optarg);
      break;
    case 'm':
      mydev = atoi(optarg);
      break;
    case 'p':
      passive = 1;
      semipassive = 0;
      break;
    case 'P':
      passive = 1;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'D':
      dbg = 1;
      break;
    case 'r':
      reset = 1;
      break;
    case 'n':
      nowriteauth = 1;
      break;
    case 'z':
      nopairing = 1;
      break;
    default:
      fprintf(stderr, "unknown option %s\n", optarg);
      usage();
    }
  }

  ac -= optind;
  av += optind;

  int i = 0;
  for (i = 0 ; i < MAXACTIVITIES ; i++) {
    memset(&activitybuf[i], 0, sizeof(activity_t));
    activitybuf[i].activitynum = -1;

    ntrackpoints[i] = 0;
    memset(&trackpointbuf[i], 0, sizeof(trackpoint_t) * MAXTRACKPOINTS);
  }

  for (i = 0 ; i < MAXLAPS ; i++) {
    memset(&lapbuf[i], 0, sizeof(lap_t));
    lapbuf[i].lapnum = -1;
  }

  if ((!passive && !authfile) || ac)
    usage();
		
  if (!ANT_Init(devnum, 0)) { // should be 115200 but doesn't fit into a short
    fprintf(stderr, "open dev %d failed\n", devnum);
    exit(1);
  }
  ANT_ResetSystem();
  ANT_AssignResponseFunction(revent, ebuf);
  ANT_RequestMessage(chan, MESG_CHANNEL_STATUS_ID); //informative
  ANT_SetNetworkKeya(net, ANTSPT_KEY);
  ANT_AssignChannel(chan, chtype, net);
  // Wali: changed order of the following seq. according windows
  ANT_SetChannelPeriod(chan, period);
  ANT_SetChannelSearchTimeout(chan, srchto);
  ANT_RequestMessage(chan, MESG_CAPABILITIES_ID); //informative
  ANT_SetChannelRFFreq(chan, freq);
  ANT_SetSearchWaveform(chan, waveform);
  ANT_SetChannelId(chan, devno, devtype, manid);
  ANT_OpenChannel(chan);
  ANT_RequestMessage(chan, MESG_CHANNEL_STATUS_ID); //informative

  // everything handled in event functions
  for(;;)
    sleep(10);
}

