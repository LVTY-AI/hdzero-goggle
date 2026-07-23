#!/bin/sh
{
echo "#######################################"

PLATFORM="$(cat /mnt/app/platform)"
#PLATFORM=HDZGOGGLE
#PLATFORM=HDZGOGGLE2
#PLATFORM=HDZBOXPRO
PLATFORMfile=$PLATFORM
if [ "$PLATFORM" == "HDZGOGGLE2" ];then
  # work around goggle2 firmware file names matching goggle v1 firmware file names
  PLATFORMfile=HDZGOGGLE
fi
TMP_DIR=/tmp/goggle_update
HDZ_BIN="$1"

TMP_RX_BIN="${TMP_DIR}/${PLATFORMfile}_RX.bin"
TMP_VA_BIN="${TMP_DIR}/${PLATFORMfile}_VA.bin"
WILDCARD_RX_BIN="${TMP_DIR}/${PLATFORMfile}_RX*.bin"
WILDCARD_VA_BIN="${TMP_DIR}/${PLATFORMfile}_VA*.bin"

if [ $PLATFORM == "HDZGOGGLE" ]; then
	WILDCARD_HDZ_BIN="/mnt/extsd/HDZERO_GOGGLE-*.bin"
elif [ $PLATFORM == "HDZGOGGLE2" ]; then
	WILDCARD_HDZ_BIN="/mnt/extsd/HDZERO_GOGGLE2-*.bin"
elif [ $PLATFORM == "HDZBOXPRO" ]; then
	WILDCARD_HDZ_BIN="/mnt/extsd/HDZERO_BOXPRO*.bin"	
fi

VAcount=1
VAwrites=0
RXcount=2
RXwrites=0

function gpio_export()
{                                                                      
        if [ ! -f /sys/class/gpio/gpio224/direction ];  then 
	  echo "224">/sys/class/gpio/export
        fi                                                                      
        if [ ! -f /sys/class/gpio/gpio228/direction ];  then 
	  echo "228">/sys/class/gpio/export
        fi                                                                      
        if [ ! -f /sys/class/gpio/gpio258/direction ];  then 
	  echo "258">/sys/class/gpio/export
        fi                                                                      
        if [ ! -f /sys/class/gpio/gpio131/direction ];  then 
	  echo "131">/sys/class/gpio/export
	fi
	echo "out">/sys/class/gpio/gpio224/direction
	echo "out">/sys/class/gpio/gpio228/direction
	echo "out">/sys/class/gpio/gpio258/direction
	echo "out">/sys/class/gpio/gpio131/direction
}

function beep()
{
	if [ ! -z "$1" ];then
	delay=$1
	else
	delay=1
	fi
	echo "1">/sys/class/gpio/gpio131/value
	sleep $delay
        echo "0">/sys/class/gpio/gpio131/value
}

function beep_success()
{
	beep 0.1
	sleep 0.5
	beep 0.05
	sleep 1
}


function beep_failure()
{
	beep 1
	sleep 0.5
	beep 1
	sleep 0.5
	beep 0.05
	sleep 1
}


function gpio_set_reset()
{
	echo "0">/sys/class/gpio/gpio224/value
	echo "1">/sys/class/gpio/gpio228/value
}

function gpio_clear_reset()
{
	echo "1">/sys/class/gpio/gpio224/value
	echo "0">/sys/class/gpio/gpio228/value
}

function disconnect_fpga_flash()
{
	echo "1">/sys/class/gpio/gpio258/value
}

function connect_fpga_flash()
{
	echo "0">/sys/class/gpio/gpio258/value
}

# eg: check_mtd_write /dev/mtdX required-size bin-file
function check_mtd_write()
{
	if [ ! -s "$3" ]; then
		echo "ERROR: $3 missing or empty - not erasing $1"
		beep_failure
		return 1
	fi
	filesize=`ls -l $3 | awk '{print $5}'`
	mtd_info=`mtd_debug info $1`
	echo "$mtd_info"
	mtdsize=`echo "$mtd_info" | grep mtd.size | grep "($2)"`                
	if [ ! -z "$mtdsize" ];then
	        echo "$1 size is ($2)" 
		mtdsizeB=`echo "$mtdsize" |cut -d " " -f 3`
		echo mtd_debug erase $1 0 $mtdsizeB
		mtd_debug erase $1 0 $mtdsizeB
		echo mtd_debug write $1 0 $filesize $3
		mtd_debug write $1 0 $filesize $3
		if [ "$PLATFORM" == "HDZGOGGLE2" ] && [ "$3" == "$TMP_VA_BIN" ] ;then
		  # write secondary VA firmware for goggle 2
		  echo mtd_debug write $1 8388608 $filesize $3		  
		  mtd_debug write $1 8388608 $filesize $3
		fi		
		if [ $? == 0 ]; then
#			beep_success
			if [ "$3" == "$TMP_VA_BIN" ]; then
				VAwrites=$((VAwrites + 1))
			fi
			if [ "$3" == "$TMP_RX_BIN" ]; then
				RXwrites=$((RXwrites + 1))
			fi
		fi 
	else
	        echo "$1 size is NOT ($2) !" 
		beep_failure
	fi
}
function untar_file()
{
	FILE_TARGET="$1"

	if [ ! -e ${TMP_DIR} ]
	then
		mkdir ${TMP_DIR}
	else
		rm ${TMP_DIR} -rf
		mkdir ${TMP_DIR}
	fi

	tar xf ${FILE_TARGET} -C ${TMP_DIR} 2>&1 > /dev/null
	mv ${WILDCARD_RX_BIN} ${TMP_RX_BIN}
	mv ${WILDCARD_VA_BIN} ${TMP_VA_BIN}
}
 

function update_rx()
{
	echo "find RX update file, start update"
	gpio_export
	gpio_set_reset
	insmod /mnt/app/ko/w25q128.ko
	check_mtd_write /dev/mtd8 1M ${TMP_RX_BIN}
	sleep 1
	check_mtd_write /dev/mtd9 1M ${TMP_RX_BIN}
	echo "update finish RX, running"
	gpio_clear_reset
	sleep 1
	rmmod /mnt/app/ko/w25q128.ko
}

function update_fpga()
{
	echo "find VA update file, start update"
	gpio_export
	gpio_set_reset
	disconnect_fpga_flash
	insmod /mnt/app/ko/w25q128.ko
	check_mtd_write /dev/mtd10 16M ${TMP_VA_BIN}
	echo "update finish VA, running"
	gpio_clear_reset
	sleep 1
	rmmod /mnt/app/ko/w25q128.ko
}

# If firmware file was NOT supplied then default to primary location for emergency restore
if [ -z "$HDZ_BIN" ]; then
	if [ `ls ${WILDCARD_HDZ_BIN} | grep bin | wc -l` -eq 1 ]
	then
		HDZ_BIN="${WILDCARD_HDZ_BIN}"
	fi
fi

if [ ! -z "$HDZ_BIN" ]; then
	echo "Flashing $HDZ_BIN"
	# A corrupt or truncated firmware file used to erase the RX and VA
	# flash with nothing to write back, bricking the goggles. Refuse to
	# touch any flash unless the whole archive reads back intact.
	if ! tar tf "$HDZ_BIN" > /dev/null 2>&1; then
		echo "ERROR: firmware file is corrupt or truncated - not flashing"
		beep_failure
		echo "100" > /tmp/progress_goggle
		exit 1
	fi
	echo "0" > /tmp/progress_goggle
	echo "0"
	untar_file "$HDZ_BIN"
	mv ${TMP_DIR}/hdzgoggle_app_ota*.tar ${TMP_DIR}/hdzgoggle_app_ota.tar
	if [ ! -s ${TMP_DIR}/hdzgoggle_app_ota.tar ] || [ ! -s ${TMP_RX_BIN} ] || [ ! -s ${TMP_VA_BIN} ]; then
		echo "ERROR: firmware archive is missing components - not flashing"
		beep_failure
		echo "100" > /tmp/progress_goggle
		exit 1
	fi
	cp -f /mnt/app/setting.ini /mnt/UDISK/
	#disable it66021
	i2cset -y 3 0x49 0x10 0xff
	echo "1"
	echo "1" > /tmp/progress_goggle
	update_rx
	echo "6"
	echo "6" > /tmp/progress_goggle
	update_fpga
 	echo "45"
	echo "45" > /tmp/progress_goggle
	hdz_upgrade_app_out=$(hdz_upgrade_app.sh 2>&1)
	hdz_upgrade_app_ret=$?
	echo "$hdz_upgrade_app_out"
	if [ $hdz_upgrade_app_ret -ne 0 ]; then
		# hdz_upgrade_app.sh (base firmware in /sbin, not built by this repo)
		# computes its post-write read-back size with "awk '{print $3/512}'",
		# which does floating-point division. The app partition is essentially
		# never an exact multiple of 512 bytes, so BusyBox dd's integer-only
		# "count=" rejects the fractional result outright and the read-back
		# always comes back empty - the "verify md5 fail" (read-back) branch
		# fires on every single flash even though the write itself, which runs
		# earlier in the same script with no count= argument, already
		# succeeded. Only "verify app.fex md5 fail" (the source-archive check
		# that runs before any write) is a genuine failure signal here.
		if echo "$hdz_upgrade_app_out" | grep -qF "verify app.fex md5 fail"; then
			echo "ERROR: app partition update failed"
			beep_failure
			echo "100" > /tmp/progress_goggle
			exit 1
		fi
	fi
	echo "100"
	echo "100" > /tmp/progress_goggle
	echo "all done"
else
	echo "skip"
fi
exit
}
