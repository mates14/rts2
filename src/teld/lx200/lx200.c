/* @file Driver file for LX200 telescope
 * 
 * Based on original c library for xephem writen by Ken Shouse
 * <ken@kshouse.engine.swri.edu> and modified by Carlos Guirao
 * <cguirao@eso.org>
 *
 * This is JUST A DRIVER, you shall to consult deamon for network
 * interface.
 *
 * @author petr 
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include "../utils/hms.c"

char *port_dev;	// device name
int port;		// port descriptor
double park_dec;	// parking declination

#define PORT_TIME_OUT 5;	// port timeout

pthread_mutex_t &tel_mutex;	// lock for port access
pthread_mutex_t &mov_mutex;	// lock for moving

/* init to given port
 * 
 * @param devptr Pointer to device name
 * 
 * @return 0 on succes, -1 & set errno otherwise
 */
int init(const char *devptr)
{
  struct termios tel_termios;
  openlog( "LX200", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1 );	// open syslog
  port_dev = malloc( strlen( devptr ) + 1 );
  strcpy( port_dev, devptr );
  port = open( port_dev, O_RDWR );
  if ( open == -1 ) 
	  return -1;

  tel_mutex = PTHREAD_MUTEX_INITIALIZER;
  mov_mutex = PTHREAD_MUTEX_INITIALIZER;

  if ( tcgetattr (port, &tel_termios ) == -1 )
	 return -1;
  
  if ( cfsetospeed( &tel_termios, B9600) == -1 ||
		  cfsetispeed( &tel_termios, B9600) == -1 )
	  return -1;
  tel_termios.c_iflag = IGNBRK & ~(IXON | IXOFF | IXANY);
  tel_termios.c_oflag = 0;
  tel_termios.c_cflag = ( tel_termios.c_cflag & ~(TERMIOS.CSIZE) | TERMIOS.CS8 ) & ~(TERMIOS.PARENB | TERMIOS.PARODD);
  tel_termiso.c_lflag = 0;
  tel_termios.c_cc[VMIN] = 1;
  tel_termios.c_cc[VTIME] = 5;

  if ( tcsetattr (port, TCSANOW, &tel_termios) == -1 )
    return -1;

  syslog (LOG_DEBUG, "Initialization complete" );

  return tcflush( port, TCIOFLUSH);
}

/* Reads some data directly from port
 * 
 * Log all flow as LOG_DEBUG to syslog
 * 
 * @exception EIO when there aren't data from port
 * @exception common select() exceptions
 * 
 * @param buf buffer to read in data
 * @param count how much data will be readed
 * @return -1 on failure, otherwise number of read data 
 */
int tel_read( char *buf, int count )
{
  int readed;
  fd_set pfds;
  struct timeval tv;

  tv.sec = PORT_TIME_OUT;
  tv.usec = 0;
  
  for (readed = 0 ; readed < count ; readed++ )
  {
    FD_ZERO( &pfds );
    FD_SET( port, &pfds );
    if ( select(port + 1, &pfds, NULL ,NULL , &tv) == 1)
      return -1;

    if (FD_ISSET( port, &pfds) ) {
      if ( read( port, &buf[readed], 1) == -1 )
        return -1;
    }
    else {
      syslog (LOG_DEBUG, "Cannot retrieve data from port" );
      errno = EIO; 
      return -1;
    }
  }
  return readed;
}

/* Will read from port till it encoutered # character
 * Read ending #, but doesn't return it.
 * 
 * @see tel_read() for description
 */

int tel_read_hash( char *buf, int count ) {
  int readed;
  buf[0] = 0;
  
  for (readed = 0 ; readed < count && buf[readed] != '#' ;
   readed++) {
    if ( tel_read( &buf[readed], 1 ) == -1 )
      return -1;
  }
  if ( buf[readed] == '#' )
    buf[readed] = 0;
  syslog (LOG_DEBUG, "Hash-readed:'%s'", buf );
  return readed;
}

/* Will write on telescope port string.
 *
 * @exception EIO, .. common write exceptions 
 * 
 * @param buf buffer to write
 * @param count count to write
 * @return -1 on failure, count otherwise
 */

int tel_write( char *buf, int count ) {
  syslog( LOG_DEBUG, "will write:'%s'", buf);
  return write(port, buf, count); 
}
	

/* Combine write && read together
 * Flush port to clear any gargabe.
 *
 * @exception EINVAL and other mutex exceptions
 * 
 * @param wbuf buffer to write on port
 * @param wcount write count
 * @param rbuf buffer to read from port
 * @param rcount maximal number of characters to read
 * @return -1 and set errno on failure, rcount otherwise
 */

int tel_write_read( char *wbuf, int wcount, char *rbuf, int rcount) {
  int tmp_rcount = -1;
  if ( !( errno = pthread_mutex_lock (&tel_mutex) ) )
    return -1;
  if ( tcflush(port, TCIOFLUSH) == -1)
    goto unlock; // we need to unlock
  if ( tel_write(wbuf, wcount) == -1 )
    goto unlock;
  
  tmp_rcount = tel_read( rbuf, rcount );

unlock:
  if ( !( errno = pthread_mutex_unlock (&tel_mutex) ) ) {
    syslog( LOG_EMERG, "Cannot unlock tel_mutex in tel_write_read:%s", strerror);
    return -1;
  }

  return tmp_rcount;
}

/* Combine write && read_hash together 
 *
 * @see tel_write_read for definition
 */

int tel_write_read_hash( char *wbuf, int rcount, char *wbuf, int rcount ) {
  int tmp_rcount = -1;
  if ( !( errno = pthread_mutex_lock (&tel_mutex) ) )
    return -1;
  if ( tcflush(port, TCIOFLUSH) == -1)
    goto unlock; // we need to unlock
  if ( tel_write(wbuf, wcount) == -1 )
    goto unlock;
  
  tmp_rcount = tel_read_hash( rbuf, rcount );

unlock:
  if ( !( errno = pthread_mutex_unlock (&tel_mutex) ) ) {
    syslog( LOG_EMERG, "Cannot unlock tel_mutex in tel_write_read_hash:%s", strerror);
    return -1;
  }

  return tmp_rcount;
}

/* Set slew rate. For completness?
 * 
 * This functions are there IMHO mainly for historical reasons. They
 * don't have any use, since if you start move and then die, telescope
 * will move forewer till it doesn't hurt itself. So it's quite
 * dangerous to use them for automatic observation. Better use quide
 * commands from attached CCD, since it defines timeout, which rules
 * CCD.
 *
 * @param new_rate New rate to set. Uses RATE_<SPEED> constant.
 * @return -1 on failure & set errno, 5 (>=0) otherwise
 */
int tel_set_rate( char new_rate) {
  char command[6];
  sprintf( command, "#:R%c#", new_rate );
  return tel_write( command, 5);
}

/* Start slew. Again for completness?
 * @see tel_set_rate() for speed.
 *
 * @param direction direction
 * @return -1 on failure & set errnom, 5 (>=0) otherwise
 */
int tel_start_slew( char direction) {
  char command[6];
  sprintf( command, "#:M%c#", direction );
  tel_write(command ,5);
}

/* Stop sleew. Again only for completness?
 * 
 * @see tel_start_slew for direction.	
 */
int tel_stop_slew( char direction ) {
  char command[6];
  sprintf( command, "#:Q%c#", direction );
  tel_write(command ,5);
}

/* Disconnect lx200.
 *
 * @return -1 on failure and set errno, 0 otherwise
 */
int tel_disconnect(void) {
 return close(port);
}

/* Reads some value from lx200 in hms format
 * Utility function for all those read_ra and other.
 * 
 * @param hmsptr where hms will be stored
 * @return -1 and set errno on error, otherwise 0
 */
int tel_read_hms(double *hmsptr, char *command) {
  char wbuf[11];
  if ( tel_write_read_hash(command, strlen(command), wbuf, 10) == -1)
    return -1;
  *raptr = hmstod(wbuf);
  if (errno)
    return -1;
  return 0;
}

/* Reads lx200 right ascenation.
 * @param raptr where ra will be stored
 * @return -1 and set errno on error, otherwise 0
 */
int tel_read_ra(double *raptr) {
  return tel_read_hms(decptr, "#:GR#");
}

/* Reads LX200 declination.
 * @param decptr where dec will be stored 
 * @return -1 and set errno on error, otherwise 0
 */
int tel_read_dec(double *decptr) {
  return tel_read_hms(decptr, "#:GD#");
}

/* Returns LX200 local time.
 * @param tptr where time will be stored
 * @return -1 and errno on error, otherwise 0
 */
int tel_read_localtime(double *tptr) {
  return tel_read_hms(decptr, "#:GL#");
}

/* Returns LX200 local time.
 * @param tptr where time will be strored
 * @return -1 and errno on error, otherwise 0
 */
int tel_read_localtime(double *tptr) {
  return tel_read_hms(decptr, "#:GL#");
}

/* Returns LX200 sideral time.
 * @param tptr where time will be stored
 * @return -1 and errno on error, otherwise 0
 */
int tel_read_sideraltime(double *tptr) {
  return tel_read_hms(decptr, "#:GS#");
}

/* Reads LX200 latitude.
 * @param latptr where latitude will be stored  
 * @return -1 and errno on error, otherwise 0
 */
int tel_read_latitude(double *tptr) {
  return tel_read_hms(decptr, "#:Gt#");
}

/* Reads LX200 longtitude.
 * @param latptr where longtitude will be stored  
 * @return -1 and errno on error, otherwise 0
 */
int tel_read_longtitude(double *tptr) {
  return tel_read_hms(decptr, "#:Gg#");
}

/* Repeat LX200 write
 * Handy for setting ra and dec.
 * Meade tends to have problems with that.
 *
 * @param command command to write on port
 */
int tel_rep_write(command) {
  int count;
  char retstr;
  for (count = 0; count < 200; count++) {
    if ( tel_write_read(command, strlen(command), &retstr, 1) == -1 )
      return -1;
    if ( retstr == '1')
      break;
    sleep(1);
    syslog( LOG_DEBUG, "tel_rep_write - for %i time.", count);
  }
  if ( count == 200 ) {
    syslog( LOG_ERR, "tel_rep_write unsucessful due to incorrect return.");
    errno = EIO;
    return -1;
  }
  return 0;
}

/* Set LX200 right ascenation
 * @param ra right ascenation to set in decimal hours
 * @return -1 and errno on error, otherwise 0
 */
int tel_write_ra(double ra) {
  char command[14];
  int h,m,s;
  if ( ra<0) 
    ra = floor( ra/24 ) * -24 + ra; //normalize ra
  if ( ra>24 )
    ra = ra - floor (ra/24) * 24;
  h = floor(ra);
  m = floor((ra-h)*60);
  s = floor((ra-h-m/60)*3600);  
  if ( snprintf( command, 13, "#:Sr%02d:%02d:%02d#", h, m ,s) == -1)
    return -1;
  return tel_rep_write(command);
}

/* Set LX200 declination
 * @param dec declination to set in decimal degrees
 * @return -1 and errno on error, otherwise 0
 */
int tel_write_dec(double dec) {
  char command[15];
  int h,m,s;
  if ( ra<0 ) 
    ra = floor( ra/90 ) * -90 + ra; //normalize dec
  if ( ra>90 )
    ra = ra - floor (ra/90) * 90;
  h = floor(dec);
  m = floor((dec-h)*60);
  s = floor((dec-h-m/60)*3600);
  if ( snprintf( command, 14, "#:Sd%+02d\xdf%02d:%02d#", h, m ,s) == -1)
    return -1;
  return tel_rep_write(command);
} 

/* Move LX200 to new coordinates
 * @param ra new right ascenation
 * @param dec new declination
 * @exception EINVAL When telescope is below horizont, or upper limit was reached
 * @return -1 and errno on exception, otherwise 0
 */
int tel_move_to(ra, dec) {
  char retstr;
  int max_read = 200;  // maximal read till # is encountered
  if (tel_write_ra(ra) == -1 || tel_write_dec(dec) == -1)
    return -1;
  if (tel_write_read("#:MS#", 5, &retstr, 1) == -1) 
    return -1;
  if ( retstr == '0' ) 
    return 0;
  retstr = 0;
  while ( max_read > 0 && retstr !='#' ) 
    if ( tel_read(&retstr, 1) == -1 )
      return -1;
  errno = EINVAL;
  return -1;


/* Check, if telescope match given coordinates
 *
 * @param ra target right ascenation
 * @param dec target declination
 * @return -1 and errno on exception, 0 if matched, 1 if not matched
 */
int tel_check_coords(ra, dec) {
  double err_ra, err_dec;
  if ( (tel_read_ra(&err_ra) == -1) || (tel_read_dec(&err_dec) == -1) )
    return -1;
  err_ra -= ra;
  err_dec -= dec;
  if (fabs(err_ra) > 0.05 or fabs(err_dec) > 0.5) 
    return 1; 
  return 0;
}
	def _moveto(self,newRA,newDec):
		"Internal, will moto to new RA and dec"
		self._moving_lock.acquire()
		try:
			i=self.slewtocoords(newRA,newDec)
			if (i):
				rtlog.log('telescope','Invalid movement: RA:'+str(newRA)+' DEC:'+str(newDec))
				raise TelescopeInvalidMovement
				return i
			self.silence=1
			timeout=time.time()+100 # timeout, wait 100 seconds to reach target
			while (time.time()<timeout) and (not(self.checkcoords(newRA,newDec))):
				# wait for a bit, telescope get confused?? when you check for ra and dec often and sometime doesn't move to required location
				# this is just a theory, which has to be proven
				time.sleep(2)
				self.silence=0
			if time.time()>timeout:
				msg="Timeout during moving to ra="+str(newRA)+" dec="+str(newDec)+". Actual ra="+str(self.getra())+" dec="+str(self.getdec())
				self._log("telescope",msg)
				raise TelescopeTimeout,msg
		finally:
			self._moving_lock.release()
			
		# need to sleep a while, waiting for final precise adjustement of the scope, which could be checked by checkcoords - the scope newer gets to precise position, it just get to something about that, so we could program checkcoords to check for precise position.  
		time.sleep(5)
		return 0

	def moveto(self,newRA,newDec):
		"""Will move to and wait for completition
		Returns 0: error
						1: succeded"""
		self._log("telescope","moveto: RA:"+str(newRA)+" Dec:"+str(newDec))
		for i in range(0,5):
			try:
				self._moveto(newRA+i*0.05,newDec)
				self._log("telescope","moveto finished")
				return 1 
			except TelescopeInvalidMovement:
				return 0
			except TelescopeTimeout:
				self._log("telescope",str(i)+". timeout during moveto "+str(newRA)+" "+str(newDec))

		msg="Too much timeouts during moving to ra="+str(newRA)+" dec="+str(newDec)+". Actual ra="+str(self.getra())+" dec="+str(self.getdec()+", exiting")
		self._log("telescope",msg)
		raise TelescopeError,msg	
	
	def setto(self,ra,dec):
		"Will set ra and dec) to given values"
		self._setra(ra)
		self._setdec(dec)
		a=self._writereadhash("#:CM#",100)
		if not(self.checkcoords(ra,dec)):
			raise TelescopeError,"Ra and dec not set!! Ra="+str(ra)+" Dec="+str(dec)
		
	
int init(char *port)
{
		"""Is called after sucessfull detection of telescope to set needed values"""
		# we get 12:34:4# while we're in short mode
		# and 12:34:45 while we're in long mode
		a=self._writeread("#:Gr#",8)
		if a[7]=="#":	# could be used to distinquish between long and
									# short mode
			self._writeread("#:U#",0)
		# now set low precision, e.g. we won't wait for user
		# to find a star
		# a="HIGH"
		# while a=="HIGH":
		# 	a=self._writeread("#:P#",4)

	def is_ready(self):
		"Will query if telescope present. If it's, call init routines"
		try:
			a=self._writeread("#:Gc#",5)
			if a not in ('(12)#','(24)#'):
				return 0 
			self.init()	
		except TelescopeError:
			return 0
		return 1

int park(void)
{
	return moveto(0,park_dec);
}
