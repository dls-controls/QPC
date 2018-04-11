/* devVacSen.c
 vacSen Vacuum Gauge Controller
 INP has form @asyn(port address) devType CC_station
 port is the asyn serial port name
 address is the controller address (0 for RS232 and between 1-31/59 for RS485)
 devType is "GP307" or "GP350" or "MM200" for Televac
 	**only for MM200 **
	CC_station is the station #  for Cold Cathode, 
	if CC is either 5/6 then the corresponding CV1 are 1/2 and CV2 are 3/4
	if CC is either 3/4 then the corresponding CV is 1/2 and no CV2
	
Revision 1.0  - 4/1/2013 
**only for MM200 **	
	removed the predetermined combination of CC and CV
	Explicitly state the station numbers in the startup file
	order is :  CC,CV1,CV2,SP1
	For setpoint lets assume odd or even based on the first setpoint 
	and has to 1 or 2.
	

Revision - 7/30/2014
    Added CC-10  from Televac  similar to MM200
        Single device which covers the range of 10^3  to 10^-9 in one gauge

    Fixed connecton problems with MOXA
        will work correctly when MOXA is rebooted now

Revision - 3/22/2016
    Added support for MX200 very similar to MM200 but the comm and front panels 
    are different. Can handle high speed comm speeds of 115K baud and also USB.
    Will use the options similar to MM200 for configuration and label this MX200
    
    This version only supports 4 setpoints per CC 
    based on the starting no of the setpoint its either even or odd..

*/

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <math.h>

#include <dbScan.h>
#include <dbDefs.h>
#include <dbAccess.h>
#include <dbCommon.h>
#include <errlog.h>
#include <alarm.h>
#include <link.h>
#include <recGbl.h>
#include <recSup.h>
#include <devSup.h>
#include <epicsString.h>
#include <asynDriver.h>
#include <asynEpicsUtils.h>
#include <asynOctet.h>
#include <vsRecord.h>
#include <epicsExport.h>

#include "devVacSen.h"

#define vacSen_BUFFER_SIZE 80
#define vacSen_SIZE 15
#define vacSen_READ_SIZE 55
#define vacSen_TIMEOUT 3.0

/* from vsRecord.c*/
#define IG1_FIELD	0x0001
#define IG2_FIELD	0x0002
#define DGS_FIELD	0x0004

typedef struct devVacSenPvt {
    asynUser    *pasynUser;
    asynOctet   *pasynOctet;
    void        *octetPvt;
    asynStatus  status;
    char        recBuf[vacSen_BUFFER_SIZE];
    char        sendBuf[vacSen_SIZE];
    char        *PortName;
    int         devType;
    char        address[3];
    int         command;
    char        cmdPrefix[4];
    int         cc;
    int         cv1;
    int         cv2;
    int         noSPT;
    int         spt;
    int         errCount;
} devVacSenPvt;

typedef struct dsetVacSen{
    long      number;
    DEVSUPFUN report;
    DEVSUPFUN init;
    DEVSUPFUN init_record;
    DEVSUPFUN get_ioint_info;
    DEVSUPFUN readWrite_vs;
} dsetVacSen;


static void devVacSenCallback(asynUser *pasynUser);
static void devVacSenWriteRead(asynUser *pasynUser, char *sendBuffer, char *readBuffer, int *nread);

static long init(vsRecord *pr);
static long readWrite_vs(vsRecord *pr);

dsetVacSen devVacSen = {5,0,0,init,0,readWrite_vs};
epicsExportAddress(dset,devVacSen);


static long init(vsRecord *pr)
{
    asynUser *pasynUser=NULL;
    asynStatus status;
    asynInterface *pasynInterface;
    devVacSenPvt *pPvt=NULL;
    DBLINK *plink  = &pr->inp;
    int address;
    char *port, *userParam;
    int station,stationC1, stationC2,spt;
    int i;
    char stx[2];
   
    /* Allocate private structure */
    pPvt = calloc(1, sizeof(devVacSenPvt));

    /* Create an asynUser */
    pasynUser = pasynManager->createAsynUser(devVacSenCallback, 0);
    pasynUser->userPvt = pr;

    /* Parse input link */
    status = pasynEpicsUtils->parseLink(pasynUser, plink, &port, &address, &userParam);

    strcpy(pPvt->cmdPrefix,"");
    strcpy(pPvt->address,"");
    pPvt->PortName = port;
    pPvt->devType = pr->type;
    /* station = CC, stationC1 = CV1, stationC2 = CV2, spt = station spt */
    i = sscanf(userParam,"%1d %1d %1d %1d",&station,&stationC1,&stationC2,&spt);
    pPvt->noSPT = 8;

/*  for MM200 for backward compatability when only CC station is defined... */
    if (pPvt->devType == 2 && i  == 1) {
        switch (station) {
            case 3:
            case 4:
                stationC1 = station-2;
                stationC2 = 0;
            break;
            case 5:
            case 6:
                stationC1 = station-4;
                stationC2 = station-2;
            break;
            default:
                printf("devVacSen::init %s station out of range %d\n",
                                                  pr->name, station);
                goto bad;
        }
        spt = stationC1;
    }
    

/*
* devType needed to be set to device as follows in record "TYPE":
*	GP307	=	0
*	GP350	=	1
*	MM200	=	2
*	CC10	=	3
*   	MX200   =   4
*
*  If address is 0 then it is RS232
*
*  For RS485 Address has to be a positive number
*	for GP350 has to be between 1 and 31 of the form "AA"
* 	for MM200 has to be between 0 and 59 of the form [0..9..A..Z..a..z]
*   for GP350 the prefix is "#" so we will force for MM200 the same!!
*	for CC10 has to be between 0 and F  in HEX
*	for CC10  the prefix is <STX>  = hex 02
*   for MX200 has to be between 00 and 99 in decimal 2 digit
*   for MX200  the prefix is "*"
*/
    if ( pPvt->devType == 1) {
        if (address > 0) 
            sprintf(pPvt->address,"%02X",address);
        if (address < 0 || address > 31) {
            errlogPrintf("devVacSen::init %s address out of range %s\n",
                   pr->name, pPvt->address);
            goto bad;
        }
        sprintf(pPvt->cmdPrefix,"#%s",pPvt->address);

    } else if (pPvt->devType == 2) {
        if (address < 0 || address > 59) {
            errlogPrintf("devVacSen::init %s address out of range %d\n",
                   pr->name, address);
            goto bad;
        }
        if (address > 0 && address <10)	
            sprintf(pPvt->address,"%c",address+48);
        if (address > 9 && address <33) 
            sprintf(pPvt->address,"%c",address+55);
        if (address >32 && address <35) 
            sprintf(pPvt->address,"%c",address+56);
        if (address >34 && address <58) 
            sprintf(pPvt->address,"%c",address+62);
        if (address >57 && address <60) 
            sprintf(pPvt->address,"%c",address+63);
	
        if (address > 0) 
            sprintf(pPvt->cmdPrefix,"#%s",pPvt->address);
        if (station < 3 || station > 9) {
            errlogPrintf("devVacSen::init %s station for CC out of range %d\n",
                   pr->name, station);
            goto bad;
        }
        pPvt->cc=station;
        pPvt->spt=0;
        if ( (stationC1 < 1) || (stationC1 > 6) ) {
	        pPvt->cv1 = 0;
        } else {
	        pPvt->cv1=stationC1;
        }
	
        if ( (stationC2 < 1) || (stationC2 > 6) ) {
	        pPvt->cv2 = 0;
        } else {
	        pPvt->cv2=stationC2;
        }
      /* Check the setpoint is in range 
        if ( spt<1 || spt > pPvt->noSPT ) { */
        
      /* at this time to read all 8 spt let the first be either 1 or 2 */
        if ( spt<1 || spt > 2 ) {        
            errlogPrintf("devVacSen::init %s setpoint out of range %d\n",
                   pr->name, spt);
            goto bad;
        }
        pPvt->spt = spt;


    } else if (pPvt->devType == 3) {
	    strcpy(stx,"\02");
	    sprintf(pPvt->address,"%1X",address);
	    if (address < 0 || address > 15) {
	        errlogPrintf("devVacSen::init %s address out of range %s\n",
                   pr->name, pPvt->address);
	        goto bad;
	    }
	    sprintf(pPvt->cmdPrefix,"%c%1X",*stx,address);


    } else if (pPvt->devType == 4) {
        if (address < 0 || address > 99) {
            errlogPrintf("devVacSen::init %s address out of range %s\n",
                   pr->name, pPvt->address);
            goto bad;
        }
        if (address > 0) {
            sprintf(pPvt->address,"%02X",address);
            sprintf(pPvt->cmdPrefix,"*%s",pPvt->address);
        }
        
        if (station < 3 || station > 9) {
            errlogPrintf("devVacSen::init %s station for CC out of range %d\n",
                   pr->name, station);
            goto bad;
        }
        pPvt->cc=station;
        pPvt->spt=0;
        if ( (stationC1 < 1) || (stationC1 > 6) ) {
	        pPvt->cv1 = 0;
        } else {
	        pPvt->cv1=stationC1;
        }
	
        if ( (stationC2 < 1) || (stationC2 > 6) ) {
	        pPvt->cv2 = 0;
        } else {
	        pPvt->cv2=stationC2;
        }
      /* Check the setpoint is in range 
        if ( spt<1 || spt > pPvt->noSPT ) { */
        
      /* at this time to read all 8 spt let the first be either 1 or 2 */
        if ( spt<1 || spt > 2 ) {        
            errlogPrintf("devVacSen::init %s setpoint out of range %d\n",
                   pr->name, spt);
            goto bad;
        }
        pPvt->spt = spt;
    }

    if (status != asynSuccess) {
        errlogPrintf("devVacSen::init %s bad link %s\n",
                   pr->name, pasynUser->errorMessage);
        goto bad;
    }

    status = pasynManager->connectDevice(pasynUser,pPvt->PortName,0);
    if(status!=asynSuccess) goto bad;

    pasynInterface = pasynManager->findInterface(pasynUser,asynOctetType,1);
    if(!pasynInterface) goto bad;
   
    pPvt->pasynOctet = (asynOctet *)pasynInterface->pinterface;
    pPvt->octetPvt = pasynInterface->drvPvt;
    pPvt->pasynUser = pasynUser;
    pr->dpvt = pPvt;

    asynPrint(pasynUser, ASYN_TRACE_FLOW,
         "devVacSen::init name=%s  port=%s address= %s Device = %d station = %d\n", 
         pr->name, pPvt->PortName, pPvt->address, pPvt->devType,station);
    return 0;

bad:
    if(pasynUser) pasynManager->freeAsynUser(pasynUser);
    if(pPvt) free(pPvt);
	errlogPrintf("devVacSen::init %s Problem initializing - quiting \n",
                   pr->name);
    pr->pact = 1;
    return -1;
}

static long readWrite_vs(vsRecord *pr)
{
    devVacSenPvt *pPvt = (devVacSenPvt *)pr->dpvt;
    asynUser *pasynUser = pPvt->pasynUser;
    asynStatus status;
    cmdType type;
    int cmd=0;
    int i;
    char data[15];
    int value=0;
    float fvalue=0;
    char sign;
    int exp;

    if (!pr->pact) {

	    memset(pPvt->sendBuf, 0, vacSen_SIZE);
	
        if (pr->chgc) {
            if (pr->chgc & IG1_FIELD)
                cmd = (int) pr->ig1s;
            else if (pr->chgc & IG2_FIELD)
                cmd = 2 + (int) pr->ig2s;
            else if (pr->chgc & DGS_FIELD) 
                cmd = 4 + (int) pr->dgss;

            pr->chgc = 0;

            type = cmdControl;
      /* depending of devType offset the commands  by multiples of 10.. */
            strcpy( pPvt->sendBuf,pPvt->cmdPrefix);
            strcat( pPvt->sendBuf,ctlCmdString[cmd+(pPvt->devType *10)]);

       /*  For MM200, MX200 and CC10 there are no commands to send. */
            if (pPvt->devType == 2 || pPvt->devType == 3 || pPvt->devType == 4)
                type = cmdRead;

        }  else {
            type = cmdRead;
        }
        
        pPvt->command = type;
 
        asynPrint(pPvt->pasynUser, ASYN_TRACEIO_DEVICE,
          "devVacSen::readWrite %s command %d len=%d string=|%s|\n",
          pr->name, pPvt->command, (int)strlen(pPvt->sendBuf), pPvt->sendBuf);
	
	    pr->pact = 1;
	    status = pasynManager->queueRequest(pasynUser, 0, 0);
        if (status != asynSuccess) {
            asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "devVacSen::readWrite %s ERROR calling queueRequest status=%d, error=%s\n",
		        pr->name, status, pasynUser->errorMessage);
	        status = -1;
	        pr->pact = 0;
     	    recGblSetSevr(pr, COMM_ALARM, INVALID_ALARM);
        }
        return(status);
    }
/*  
*	Now process the data back from the device during callback 
*	for GP307 and GP350 pack the data differently than for MM200
*	and for CC10  similar to MM200 but different
*   Mx200 is simliar to MM200 but different
*
*	check the error count.
*	if errCount = 0 then process normally.  
*	If errCount >= ERR record field then set the invalid read alarm and return.
*	if errCount >0  & < ERR then skip writing to fields.
*/
    if (pPvt->errCount >= pr->err) {
        recGblSetSevr(pr, READ_ALARM, INVALID_ALARM);
        pr->udf=0;
        return(0);
    }

    if ( (pPvt->errCount > 0) && (pPvt->errCount < pr->err) ) {
        pr->udf=0;
        return(0);
    } 
 
    pr->dgsr = 0;
    pr->ig1r = 0;
    pr->ig2r = 0;
    pr->val = 9.9e+9;

    /*  for GP 307 and GP350 similar commands */
    if (pPvt->devType < 2) {
        switch (pPvt->devType) {
	    case 1:
            strncpy(data,&pPvt->recBuf[0],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp1 = value;
            strncpy(data,&pPvt->recBuf[1],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp2 = value;
            strncpy(data,&pPvt->recBuf[2],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp3 = value;
            strncpy(data,&pPvt->recBuf[3],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp4 = value;
        break;
	    case 0:
            strncpy(data,&pPvt->recBuf[0],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp1 = value;
            strncpy(data,&pPvt->recBuf[2],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp2 = value;
            strncpy(data,&pPvt->recBuf[4],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp3 = value;
            strncpy(data,&pPvt->recBuf[6],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp4 = value;
            strncpy(data,&pPvt->recBuf[8],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp5 = value;
            strncpy(data,&pPvt->recBuf[10],2);
            data[1]=0;
            sscanf(data,"%d",&value);
            pr->sp6 = value;
        break;
        }

      /*  degas is either 0 or 1 */
        if(pPvt->recBuf[15] == '1')
	        pr->dgsr = 1;
      /* for 4 pressure values  of the x.xxE-(+)yy */
        for (i = 2; i<6; i++) {    	
	        strncpy(data,&pPvt->recBuf[10*i],10);
	        data[8] = 0;
	        sscanf(data,"%e",&fvalue);
	        switch (i) {
            	case 2:
		        if (fvalue < 1.0) {
                		pr->ig1r = 1;
                		pr->val = (double) fvalue;
		        }
		    	break;
            	case 3:
		        if (fvalue < 1.0) {
                		pr->ig2r = 1;
                		pr->val = (double) fvalue;
		        }
		    	break;
            	case 4:
                	pr->cgap = (double) fvalue;;
		    	break;
            	case 5:
                	pr->cgbp = (double) fvalue;;
		    	break;
	        }
        }
            
    /*  for MM200 commands */
    } else if (pPvt->devType ==2) {
      /*  process the setpoint "xx" */        
        if(pPvt->recBuf[0] == 'n')
            pPvt->recBuf[0] = '0';
        if(pPvt->recBuf[1] == 'n')
  	        pPvt->recBuf[0] = '0';
        strncpy(data,&pPvt->recBuf[0],3);
        data[2] =0;
        sscanf(data, "%x", &value);
      /* Set SP# to On/Off values This code is wrong!! reads as is...
        pr->sp1 = (value & 0x01 && pPvt->spt == 1 ?1:0);
        pr->sp2 = (value & 0x02 && pPvt->spt == 2 ?1:0);
        pr->sp3 = (value & 0x04 && pPvt->spt == 3 ?1:0);
        pr->sp4 = (value & 0x08 && pPvt->spt == 4 ?1:0);
*/

/*      We need to check the starting spt and offset by 2 each for 4 spts
        correcponding to the values we read on the setpoints.
*/      
        pr->sp1 = (value >> (pPvt->spt-1)) & 1;
        pr->sp2 = (value >> (pPvt->spt+1)) & 1;
        pr->sp3 = (value >> (pPvt->spt+3)) & 1;
	pr->sp4 = (value >> (pPvt->spt+5)) & 1;


        for (i = 1; i<8; i++) {
        /* process the cc,cv1 and (cv2 if exists) n=x.xx-(+)eT */
            if (i < 4 ) {
                strncpy(data,&pPvt->recBuf[10*i+2],8);
                if (data[6] == 'T') {
                    data[6] =0;
                    sscanf(data,"%f%c%x",&fvalue,&sign,&exp);
                } else {
                    fvalue = 9.9;
                    sign = '+';
                    exp=9; 
                }
  	    /* process the setpoint values x.x-(+)e  or x.xx-(+)e*/
            } else {
                strncpy(data,&pPvt->recBuf[10*i],10);
                if (data[3] == '-' || data[3] == '+') {
                    data[5] =0;
                    sscanf(data,"%f%c%x",&fvalue,&sign,&exp);
                } else if (data[4] == '-' || data[4] == '+'){
                    data[6] =0;
                    sscanf(data,"%f%c%x",&fvalue,&sign,&exp);
                } else {
                    fvalue = 0;
                    sign = '+';
                    exp=0; 
                }
            }

            if (sign == '-')
                exp -= exp*2;
            fvalue = fvalue * pow(10,exp);
            switch (i) {
            case 1:
                pr->val = (double) fvalue;
 		        if (fvalue < 1.0)
                    pr->ig1r = 1;
            break;
            case 2:
                pr->cgap = (double) fvalue;
            break;
            case 3:
                pr->cgbp = (double) fvalue;
            break;
            case 4:
                pr->sp1r = (double) fvalue;
            break;
            case 5:
                pr->sp2r = (double) fvalue;
            break;
            case 6:
                pr->sp3r = (double) fvalue;
            break;
            case 7:
                pr->sp4r = (double) fvalue;
            break;
            }	
        }

   /*  for CC10 commands */
    } else if (pPvt->devType ==3){

        /*  process the setpoint "xxxx" */
              
	    strncpy(data,&pPvt->recBuf[0],2);
	    data[1]=0;
	    sscanf(data,"%d",&value);
	    pr->sp1 = value;
	    strncpy(data,&pPvt->recBuf[1],2);
	    data[1]=0;
	    sscanf(data,"%d",&value);
	    pr->sp2 = value;
	    strncpy(data,&pPvt->recBuf[2],2);
	    data[1]=0;
	    sscanf(data,"%d",&value);
	    pr->sp3 = value;
	    strncpy(data,&pPvt->recBuf[3],2);
	    data[1]=0;
	    sscanf(data,"%d",&value);
	    pr->sp4 = value;

	/* for 4 pressure values  of the abcd where a.b^10-(+)d where c=0 is - and c=1 is + */
	    for (i = 1; i<5; i++) {    	
	        strncpy(data,&pPvt->recBuf[10*i],10);
	        data[4] = 0;
	        sscanf(data,"%2d%c%x",&value,&sign,&exp);
	        if (sign =='0') 
		        exp -= exp*2;

    	    fvalue = value/10.0 * pow(10,exp);

	        switch (i) {
		    case 1:
        	    pr->val = (double) fvalue;
		    break;
		    case 2:
        	    pr->sp1r = (double) fvalue;
 		    break;
		    case 3:
        	    pr->sp2r = (double) fvalue;
		    break;
		    case 4:
        	    pr->sp3r = (double) fvalue;
 		    break;
	        }
	    }

    /*  for MX200 commands */ 
    } else if (pPvt->devType ==4) {
      /*  process the setpoint "xx" */        
        strncpy(data,&pPvt->recBuf[0],3);
        data[2] =0;
        sscanf(data, "%x", &value);
/*      We need to check the starting spt and offset by 2 each for 4 spts
        correcponding to the values we read on the setpoints.
*/      
        pr->sp1 = (value >> (pPvt->spt-1)) & 1;
        pr->sp2 = (value >> (pPvt->spt+1)) & 1;
        pr->sp3 = (value >> (pPvt->spt+3)) & 1;
	pr->sp4 = (value >> (pPvt->spt+5)) & 1;
        
        for (i = 1; i<8; i++) {
        /* process the cc,cv1 and cv2   = ppsee
             stands for p.p x 10^see where s of 0 =- 1 =+ 
        and the setpoint of ppseePPSEE  where we ingore the second values */
		strncpy(data,&pPvt->recBuf[10*i],10);
	        data[6] = 0; /* Add NULL terminator to strng after data */
	        sscanf(data,"%2d%1d%2d",&value,&sign,&exp);
	        if (sign == 0)
			exp -= exp*2;
		        
		fvalue = value/10.0 * pow(10,exp);
		switch (i) {
		    case 1:
		    	pr->val = (double) fvalue;
	 		/*if (fvalue < 1.0)
				pr->ig1r = 1;*/
		    	break;
		    case 2:
			pr->cgap = (double) fvalue;
		    	break;
		    case 3:
			pr->cgbp = (double) fvalue;
		    	break;
		    case 4:
		        pr->sp1r = (double) fvalue;
		    	break;
		    case 5:
		        pr->sp2r = (double) fvalue;
		    	break;
		    case 6:
		        pr->sp3r = (double) fvalue;
		    	break;
		    case 7:
		        pr->sp4r = (double) fvalue;
		    	break;
		}	
	}
    }

    pr->lprs = log10(pr->val);
    pr->lcap = log10(pr->cgap);
    pr->lcbp = log10(pr->cgbp);
    pr->pres = pr->val;	  /* for backward compatability */
    pr->udf=0;
    return(0);
}


static void devVacSenCallback(asynUser *pasynUser)
{
    dbCommon *pr = (dbCommon *)pasynUser->userPvt;
    devVacSenPvt *pPvt = (devVacSenPvt *)pr->dpvt;
    char readBuffer[vacSen_READ_SIZE];
    char responseBuffer[vacSen_BUFFER_SIZE];
    struct rset *prset = (struct rset *)(pr->rset);
    int i, nread, j;
    char *pstartdata=0;
    char addcmd[3];
    char *value;

    pPvt->pasynUser->timeout = vacSen_TIMEOUT;

/*
 *  VacSen on normal cycle should get status from all the values.
 *  For commands issued the sendBuf will have a finite value.
 */
    if (pPvt->command) {
        devVacSenWriteRead(pasynUser, pPvt->sendBuf,readBuffer,&nread);
    
        if (nread < 1 ) {
            asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "devVacSen::devVacSenCallback %s Cmd reply too small=%d\n", 
                  pr->name, nread);
            goto finish;
        }
      /* for GP307 */
        if (pPvt->devType == 0) {	
            if (strcmp(readBuffer,"OK") != 0 ) {
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                    "devVacSen::devVacSenCallback %s Cmd reply has error=[%s]\n", 
                    pr->name, readBuffer);
                recGblSetSevr(pr, READ_ALARM, INVALID_ALARM);
                goto finish;
            }
      /* for GP350 */
        } else if (pPvt->devType == 1) {	
            if (readBuffer[0] =='?') {
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                    "devVacSen::devVacSenCallback %s Cmd reply has error=[%s]\n", 
                    pr->name, readBuffer);
                recGblSetSevr(pr, READ_ALARM, INVALID_ALARM);
                goto finish;
            }
        } 
    }

/*   Now start the various reads ......
 *   make sure to check the devType and send the correct set of commands
 *   devType ->  0 = GP307, 1=GP350, 2=MM200    3=CC10 4=MX200
 *   GP307 and GP350 have only 6 commands while MM200/MX200 has 8 commands
 *   CC10  has only 5 commands
 *   The data will be packed into responseBuf separated by ",".

 *   The locations are as follows for GP307 and GP350
 *   we use "PCS" for GP307 and "PC S" for GP350
 *	 307 = x,x,x,x,x,x<cr><lf>     350=# xxxx     <cr>  where x=1 or 0
 *   responseBuffer[0-14]    = SetPoint Status
 *   responseBuffer[15]      = Degas Status 0 or 1
 *   responseBuffer[20-29]   = IG1 pressure  x.xxE-xx
 *   responseBuffer[30-39]   = IG2 pressure  x.xxE-xx
 *   responseBuffer[40-49]   = CG1 pressure  x.xxE-xx
 *   responseBuffer[50-59]   = CG2 pressure  x.xxE-xx
 
 *   The locations are as follows for MM200
 *   responseBuffer[0-9]    = SetPoint Status in 2 char with 2nd char for low 4
 *   responseBuffer[10-19]  = CC pressure  n=x.xx-(+)eT
 *   responseBuffer[20-29   = CG1 pressure n=x.xx-(+)eT
 *   responseBuffer[30-39]  = CG2 pressure n=x.xx-(+)eT  ( if pressent)
 *   responseBuffer[40-49]  = SP1/2 pressure  x.x-(+)e or x.xx-(+)e
 *   responseBuffer[50-59]  = SP3/4 pressure  x.x-(+)e or x.xx-(+)e
 *   responseBuffer[60-69]  = SP5/6 pressure  x.x-(+)e or x.xx-(+)e
 *   responseBuffer[70-79]  = SP7/8 pressure  x.x-(+)e or x.xx-(+)e
 
 *   The locations are as follows for CC10 
 *   all responses start with <stx><addr><cmd>
 *   responseBuffer[0-9]    = SetPoint Status in 4 char of 0 or 1 each
 *   responseBuffer[10-19]  = CC pressure   ppse where p.p-(+)e Torr where s= 0 for - and 1 for +
 *   responseBuffer[20-29]  = SP1 pressure  ppsePPSE p.p-(+)e and P.P-(+)E for low and high
 *   responseBuffer[30-39]  = SP2 pressure  ppsePPSE p.p-(+)e and P.P-(+)E for low and high
 *   responseBuffer[40-49]  = SP3 pressure  ppsePPSE p.p-(+)e and P.P-(+)E for low and high

 *   The locations are as follows for MX200
 *   responseBuffer[0-9]    = 01=xx 02=xx ...08=xx where xx can be ON OF 00
 *   responseBuffer[10-19]  = CC pressure  ppsee where p.p x 10^see wher s of 0 =- 1 =+
 *   responseBuffer[20-29   = CG1 pressure 
 *   responseBuffer[30-39]  = CG2 pressure 
 *   responseBuffer[40-49]  = SP1/2 pressure  ppseePPSEEZZ p.p-(+)e and P.P-(+)E for low and high
 *   responseBuffer[50-59]  = SP3/4 pressure  ppseePPSEEZZ p.p-(+)e and P.P-(+)E for low and high
 *   responseBuffer[60-69]  = SP5/6 pressure  ppseePPSEEZZ p.p-(+)e and P.P-(+)E for low and high
 *   responseBuffer[70-79]  = SP7/8 pressure  ppseePPSEEZZ p.p-(+)e and P.P-(+)E for low and high


*/ 
    memset(responseBuffer, 0, vacSen_BUFFER_SIZE);

    for (i=0;i<8;i++) {
      /*  check for GP307 and GP350 and exit when commands are done */	
        if (i > 5 && pPvt->devType < 2)  continue;

      /*  check for CV2 nonexistance and skip*/	
        if (pPvt->devType ==2 && i==3 && pPvt->cv2==0) continue;

      /* for MM200 if no of setpoints is only 2 then skip */
      /*if (pPvt->devType ==2 && pPvt->noSPT==2 && i>5) continue;*/

      /*  for CC10 read gauges only once and go to setpoints  */
        if (pPvt->devType ==3  && i>4) continue;
	
        strcpy( pPvt->sendBuf,pPvt->cmdPrefix);
        strcat( pPvt->sendBuf,readCmdString[i+(pPvt->devType *10)]);

      /* for MM200 we have to add the station number to command */
        if (pPvt->devType == 2) {
            switch (i) {
            case 1:
                sprintf(addcmd,"%d",pPvt->cc);
            break;
            case 2:
                sprintf(addcmd,"%d",pPvt->cv1);
            break;
            case 3:
                sprintf(addcmd,"%d",pPvt->cv2);
            break;
            case 4:
                sprintf(addcmd,"%dN",pPvt->spt);
            break;
            case 5:
                sprintf(addcmd,"%dN",(2 + pPvt->spt));
            break;
            case 6:
                sprintf(addcmd,"%dN",(4 + pPvt->spt));
            break;
            case 7:
                sprintf(addcmd,"%dN",(6 + pPvt->spt));
            break;
            default:
                strcpy(addcmd,"");
            break;
            }
            strcat(pPvt->sendBuf,addcmd);
        }

      /* for MX200 we have to add the station number to command */
        if (pPvt->devType == 4) {
            switch (i) {
            case 1:
                sprintf(addcmd,"%02d",pPvt->cc);
            break;
            case 2:
                sprintf(addcmd,"%02d",pPvt->cv1);
            break;
            case 3:
                sprintf(addcmd,"%02d",pPvt->cv2);
            break;
            case 4:
                sprintf(addcmd,"%d",pPvt->spt);
            break;
            case 5:
                sprintf(addcmd,"%d",(2 + pPvt->spt));
            break;
            case 6:
                sprintf(addcmd,"%d",(4 + pPvt->spt));
            break;
            case 7:
                sprintf(addcmd,"%d",(6 + pPvt->spt));
            break;
            default:
                strcpy(addcmd,"");
            break;
            }
            strcat(pPvt->sendBuf,addcmd);
        }


/*  Send the command to the device and read the response */
        devVacSenWriteRead(pasynUser, pPvt->sendBuf,readBuffer,&nread);
        if (nread < 1 || nread > 50) {
            asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "devVacSen::devVacSenCallback %s Read reply too small/too large =%d\n", 
                  pr->name, nread);
            pPvt->errCount++;
            goto finish;
        }

/*
*   For Problems:
*     For GP307 series sends out message with the words ERROR at the end
*     For GP350 series all error messages start with ? instead of *
*     Lets look for these and set the alarm on the record if problems.
*     For GP350 lets strip the leading character and the space [*]
*     For MM200 the reply is "x?" where x=ACDLNORS
*     FOR CC10  the reply is "<STX><addr>N000x<CR>
*     For MX200  the reply is "0N000x"
*/              
      /* for GP307 */	
        if (pPvt->devType ==0) {
            pstartdata = &readBuffer[nread-5];
            if (strcmp(pstartdata,"ERROR") == 0 ) {
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "devVacSen::devVacSenCallback %s Read reply has error=[%s]\n", 
                   pr->name, readBuffer);
                pPvt->errCount++;
                goto finish;
            }
            pstartdata = &readBuffer[0];

      /* for GP350 */
        } else if (pPvt->devType == 1) {    
            if (readBuffer[0] =='?') {
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "devVacSen::devVacSenCallback %s Read reply has error=[%s]\n", 
                   pr->name, readBuffer);
                pPvt->errCount++;
                goto finish;
            }
        /* strip off the headers */
            pstartdata = &readBuffer[2]; 

      /* for MM200 */
        } else if (pPvt->devType == 2) {      
            if (readBuffer[1] =='?') {
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "devVacSen::devVacSenCallback %s Read reply has error=[%s]\n", 
                   pr->name, readBuffer);
                pPvt->errCount++;
                goto finish;
	        }
	    /* Figure out how many relays are there based on reply. */
	    /* The reply from a RY command is two characters (page 88 Televac manual). */
        /* Each charater is one relay board, one board has 4 relays. */
        /* A charater "n" denotes empty/no relay board. */
        /* Posible options are 0, 4 or 8 relays. */
        /* The APS uses at least one relay board. */
	        if(i==0) {
                if(readBuffer[0] == 'n')
                    pPvt->noSPT = 4;
                else 
                    pPvt->noSPT = 8;
	        }
 	        pstartdata = &readBuffer[0];

	  /* for CC10 */
        } else if (pPvt->devType == 3) {      
            if (readBuffer[2] =='N') {
		        readBuffer[0] = 'Z';
            	asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "devVacSen::devVacSenCallback %s Read reply has error=[%s]\n", 
                   pr->name, readBuffer);
	            pPvt->errCount++;
            	goto finish;
	        }
 	        pstartdata = &readBuffer[3];
  
      /* for MX200 */
        } else if (pPvt->devType == 4) {      
            if (readBuffer[1] =='N') {
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "devVacSen::devVacSenCallback %s Read reply has error=[%s]\n", 
                   pr->name, readBuffer);
                pPvt->errCount++;
                goto finish;
	        }
	    /* The device will send all the relay status in one shot. */
	    /*  strip the status of the relays and send only data */
	    /*  Result = 0 for OFF or 1 for ON non existant is also 0 */
	    /*  Send back 8 characters of either 0 or 1 */ 
	    /*  lets check starting at 4th char every 6th char for */
	    /*  either "N"  for ON or "F" for OFF  or "0"  for none */
	    /*  Then load a 1 or 0  as abit for total 8 bits...*/
	        if(i==0) {
                value =0;
                for (j=0;i<8;j++) {
                    if (readBuffer[4 +j*6] == 'N')
                        value += 1 << j;
                }
                *readBuffer = sprintf("%2x", value);

        /* For setpoint readbacks the response is 12 character */
        /* The last two characters are station no and can be striped off  */
	        } else if ( i >3 ) {
	            readBuffer[10] = 0;
	        }
 	        pstartdata = &readBuffer[0];
        }

      /* for Degas alone move the data by 5 to accomadate GP307 PCS */
        if (pPvt->devType < 2 && i==1)
	        strcpy(&responseBuffer[15],pstartdata);
        else
	        strcpy(&responseBuffer[10*i],pstartdata);
	
    }
    /* for successful read set errCount=0 */
    pPvt->errCount=0;    

/* Process the record. This will result in the readWrite_vs routine
 *       being called again, but with pact=1 
 */
finish:
    memset(pPvt->recBuf, 0, vacSen_BUFFER_SIZE);
    if (pPvt->errCount == 0)
	    memcpy(pPvt->recBuf, responseBuffer, vacSen_BUFFER_SIZE);
    dbScanLock(pr);
    (*prset->process)(pr);
    dbScanUnlock(pr);
}

static void devVacSenWriteRead(asynUser *pasynUser, char *sendBuffer, 
			char *readBuffer, int *nread)
{
    dbCommon *pr = (dbCommon *)pasynUser->userPvt;
    devVacSenPvt *pPvt = (devVacSenPvt *)pr->dpvt;
    size_t nwrite; 
    int eomReason;
                                           
    pPvt->pasynUser->timeout = vacSen_TIMEOUT;

    pPvt->status = pPvt->pasynOctet->write(pPvt->octetPvt, pasynUser, 
                            sendBuffer, strlen(sendBuffer), &nwrite);


    if (pPvt->status != asynSuccess) {
	    asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "devVacSen::devVacSenWriteRead %s pasynOctet->write, status=%d, error=%s\n",
		pr->name, pPvt->status, pasynUser->errorMessage);
    }


    asynPrint(pasynUser, ASYN_TRACEIO_DEVICE,
              "devVacSen::devVacSenWriteRead %s nwrite=%ld output=[%s]\n",
              pr->name, nwrite, sendBuffer);
    
    pPvt->status = pPvt->pasynOctet->read(pPvt->octetPvt, pasynUser, 
                            readBuffer, vacSen_READ_SIZE, &nwrite, &eomReason);

    *nread = nwrite;

    if (pPvt->status != asynSuccess) {
	    asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "devVacSen::devVacSenWriteRead %s pasynOctet->read, status=%d, error=%s\n",
		pr->name, pPvt->status, pasynUser->errorMessage);
    }
    if (nwrite == vacSen_READ_SIZE ) {
        pPvt->status = pPvt->pasynOctet->flush(pPvt->octetPvt, pasynUser);
	    asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "devVacSen::devVacSenWriteRead %s pasynOctet->flush, status=%d, error=%s\n",
		pr->name, pPvt->status, pasynUser->errorMessage);
    }

    asynPrint(pasynUser, ASYN_TRACEIO_DEVICE,
              "devVacSen::devVacSenWriteRead %s nread=%d input=[%s]\n",
              pr->name, *nread, readBuffer);

}

