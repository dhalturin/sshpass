/*  This file is part of "sshpass", a tool for batch running password ssh authentication
 *  Copyright (C) 2006 Lingnu Open Source Consulting Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version, provided that it was accepted by
 *  Lingnu Open Source Consulting Ltd. as an acceptable license for its
 *  projects. Consult http://www.lingnu.com/licenses.html
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include <unistd.h>
#include <fcntl.h>
//#include <asm/ioctls.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

int runprogram( int argc, char *argv[] );

struct {
    enum { PWT_STDIN, PWT_FILE, PWT_FD, PWT_PASS } pwtype;
    union {
	const char *filename;
	int fd;
	const char *password;
    } pwsrc;
} args;

static void show_help()
{
    printf("Usage: " PACKAGE_NAME " -fdph command parameters\n"
	    "   -f filename   Take password to use from file\n"
	    "   -d number     Use number as file descriptor for getting password\n"
	    "   -p password   Provide password as argument (security unwise)\n"
	    "   -e            Password is passed as env-var \"SSHPASS\"\n"
	    "   With no parameters - password will be taken from stdin\n\n"
	    "   -h            Show help (this screen)\n"
	    "At most one of -f, -d, -p or -e should be used\n");
}

// Parse the command line. Fill in the "args" global struct with the results. Return argv offset
// on success, and a negative number on failure
static int parse_options( int argc, char *argv[] )
{
    int error=0;
    int opt;

    // Set the default password source to stdin
    args.pwtype=PWT_STDIN;
    args.pwsrc.fd=0;

#define VIRGIN_PWTYPE if( args.pwtype!=PWT_STDIN ) { \
    fprintf(stderr, "Conflicting password source\n"); \
    error=-3; }

    while( (opt=getopt(argc, argv, "+f:d:p:he"))!=-1 && error==0 ) {
	switch( opt ) {
	case 'f':
	    // Password should come from a file
	    VIRGIN_PWTYPE;
	    
	    args.pwtype=PWT_FILE;
	    args.pwsrc.filename=optarg;
	    break;
	case 'd':
	    // Password should come from an open file descriptor
	    VIRGIN_PWTYPE;

	    args.pwtype=PWT_FD;
	    args.pwsrc.fd=atoi(optarg);
	    break;
	case 'p':
	    // Password is given on the command line
	    VIRGIN_PWTYPE;

	    args.pwtype=PWT_PASS;
	    args.pwsrc.password=optarg;
	    break;
	case 'e':
	    VIRGIN_PWTYPE;

	    args.pwtype=PWT_PASS;
	    args.pwsrc.password=getenv("SSHPASS");
	    break;
	case '?':
	case ':':
	    error=-2;
	    break;
	case 'h':
	    error=-1;
	    break;
	}
    }

    if( error==0 )
	return optind;
    else
	return error;
}

int main( int argc, char *argv[] )
{
    int opt_offset=parse_options( argc, argv );

    if( opt_offset<0 ) {
	// There was some error
	show_help();

	if( opt_offset==-1 )
	    return 0;
	else
	    return -opt_offset;
    }

    return runprogram( argc-opt_offset, argv+opt_offset );
}

void handleoutput( int fd );

int runprogram( int argc, char *argv[] )
{
    // Create a pseudo terminal for our process
    int masterpt=getpt();

    if( masterpt==-1 ) {
	perror("Failed to get a pseudo terminal");

	return 1;
    }

    if( grantpt( masterpt )!=0 ) {
	perror("Failed to change pseudo terminal's permission");

	return 1;
    }
    if( unlockpt( masterpt )!=0 ) {
	perror("Failed to unlock pseudo terminal");

	return 1;
    }

    int childpid=fork();
    if( childpid==0 ) {
	// Child

	// Detach us from the current TTY
	setsid();
	
	const char *name=ptsname(masterpt);
	int slavept=open(name, O_RDWR );
	//fprintf(stderr, "Opened %s with fd %d\n", name, slavept);
	close( masterpt );

	char **new_argv=malloc(sizeof(char *)*(argc+1));

	int i;

	for( i=0; i<argc; ++i ) {
	    new_argv[i]=argv[i];
	}

	new_argv[i]=NULL;

	execvp( new_argv[0], new_argv );

	exit(errno);
    } else if( childpid>0 ) {
	int status;
	
	do {
	    fd_set readfd;
	    
	    FD_ZERO(&readfd);
	    FD_SET(masterpt, &readfd);

	    int selret=select( masterpt+1, &readfd, NULL, NULL, NULL );

	    if( selret>0 ) {
		if( FD_ISSET( masterpt, &readfd ) ) {
		    handleoutput( masterpt );
		}
	    }
	    waitpid( childpid, &status, WNOHANG );
	} while( !WIFEXITED( status ) );

	//fprintf(stderr, "Done\n");
    } else {
    }

    return 0;
}

int match( const char *reference, const char *buffer, ssize_t bufsize, int state );
void write_pass( int fd );

void handleoutput( int fd )
{
    // We are looking for the string
    static int state;
    static const char compare[]="Password: ";
    char buffer[40];

    int numread=read(fd, buffer, sizeof(buffer) );

    state=match( compare, buffer, numread, state );

    if( compare[state]=='\0' ) {
	write_pass( fd );
	state=0;
    }
}

int match( const char *reference, const char *buffer, ssize_t bufsize, int state )
{
    // This is a highly simplisic implementation. It's good enough for matching "Password: ", though.
    int i;
    for( i=0;reference[state]!='\0' && i<bufsize; ++i ) {
	if( reference[state]==buffer[i] )
	    state++;
	else {
	    state=0;
	    if( reference[state]==buffer[i] )
		state++;
	}
    }

    return state;
}

void write_pass_fd( int srcfd, int dstfd );

void write_pass( int fd )
{
    switch( args.pwtype ) {
    case PWT_STDIN:
	write_pass_fd( STDIN_FILENO, fd );
	break;
    case PWT_FD:
	write_pass_fd( args.pwsrc.fd, fd );
	break;
    case PWT_FILE:
	{
	    int srcfd=open( args.pwsrc.filename, O_RDONLY );
	    if( srcfd!=-1 ) {
		write_pass_fd( srcfd, fd );
		close( srcfd );
	    }
	}
	break;
    case PWT_PASS:
	write( fd, args.pwsrc.password, strlen( args.pwsrc.password ) );
	write( fd, "\n", 1 );
	break;
    }
}

void write_pass_fd( int srcfd, int dstfd )
{

    int done=0;

    while( !done ) {
	char buffer[40];
	int i;
	int numread=read( srcfd, buffer, sizeof(buffer) );
	done=(numread<1);
	for( i=0; i<numread && !done; ++i ) {
	    if( buffer[i]!='\n' )
		write( dstfd, buffer+i, 1 );
	    else
		done=1;
	}
    }

    write( dstfd, "\n", 1 );
}