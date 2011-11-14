#!/bin/bash

hash make 2>&- || { echo >&2 "I require make but it's not installed.  Aborting."; exit 1; }
hash autoreconf 2>&- || { echo >&2 "I require aureconf but it's not installed.  Aborting."; exit 1; }
hash unzip 2>&- || { echo >&2 "I require unzip but it's not installed.  Aborting."; exit 1; }
hash svn 2>&- || { echo >&2 "I require svn but it's not installed.  Aborting."; exit 1; }


function errorcheck(){
    if [ "$?" -gt 0 ]; then
		echo "Error during $1"
		exit
    else
		echo "$1 successful"
    fi
}

function incusage(){
	echo "Incorrect usage; use --help for more info."
	exit
}

if [ "$1" = "reconf" ]; then
	cd vcmi
	autoreconf -i
	cd ..
	vcmi/configure --datadir=`pwd` --bindir=`pwd`vcmi --libdir=`pwd`
	exit
fi

if [ "$1" == "--install" ]; then
	if [ $# -lt 2 ]; then
		incusage
	fi
	if [ "$2" == "micro" ]; then
	    unzip vcmipack.zip -d YourAI
	    MAINVCMI_ROOT="/home/mateusz/vcmi_retl"
	    ln -s "$MAINVCMI_ROOT/odpalarka"
	    exit
	fi
	#only lean and full modes
	svn co https://vcmi.svn.sourceforge.net/svnroot/vcmi/branches/programmingChallenge/ vcmi
	errorcheck "fetching sources"
	cd vcmi
		
	if [ "$2" = "lean" ]; then
		mv "Makefile without client.am" Makefile.am
		mv "configure without client.ac" configure.ac
		rm client/Makefile.am
		echo "SUBDIRS = StupidAI" > AI/Makefile.am
		rm -rf AI/EmptyAI
		rm -rf AI/GeniusAI
		#find . -name ".svn" -exec rm -rf {} \;
	elif [ "$2" = "full" ]; then
		mv "Makefile with client.am" Makefile.am
		mv "configure with client.ac" configure.ac
	else
		incusage
	fi
	autoreconf -f
	errorcheck "autoreconf -f"
	wget http://sourceforge.net/apps/trac/vcmi/export/2443/branches/programmingChallenge/vcmipack.zip
	cd ..
	vcmi/configure --datadir=`pwd` --bindir=`pwd`vcmi --libdir=`pwd`
	errorcheck "configure"
	make
	errorcheck "make"
	unzip vcmi/vcmipack.zip -d vcmi
	errorcheck "pack unzip"
	ln -s "vcmi/b1.json"
	errorcheck "b1.json symlink"
	ln -s ../../AI/StupidAI/.libs/libStupidAI.so -t vcmi/AI
	errorcheck "StupidAI symlink"
	ln -s "Odpalarka/odpalarka"
	errorcheck "Odpalarka symlink"
	ln -s "VCMI_BattleAiHost/vcmirunner"
	errorcheck "runner symlink"
	ln -s "server/vcmiserver"
	errorcheck "server symlink"
elif [ "$1" = "--clean" ]; then
	find . -name "*" -exec bash -c "if [[ {} != './vcmiinstall.sh' ]] && [[ {} != './vcmipack.zip' ]] && [[ {} != '.' ]] && [[ {} != '..' ]]; then rm -rf {} ; fi" \;
elif [ "$1" = "--help" ]; then
	echo "VCMI programming challenge installation script"
	echo
	echo "Available commands: "
	echo "--clean			removes all created files except this script."
	echo "--help			displays this info."
	echo "--install full	downloads and compiles full VCMI with graphical client; requires ffmpeg."
	echo "--install lean	downloads and compiles most of VCMI (without graphical client)."
	echo "--install micro	unpacks vcmipack.zip and makes appropriate symbolic links."
else
	incusage
fi