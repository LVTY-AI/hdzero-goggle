#!/bin/sh

function amixer_adc()
{
        onoff=$1
        amixer cset name='AIF1 AD0L Mixer ADCL Switch' $onoff
        amixer cset name='AIF1 AD0R Mixer ADCR Switch' $onoff        
}

function amixer_mic1()
{
        onoff=$1
        amixer cset name='LADC input Mixer MIC1 boost Switch' $onoff
        amixer cset name='RADC input Mixer MIC1 boost Switch' $onoff
}

function amixer_mic2()
{
        onoff=$1
        amixer cset name='LADC input Mixer MIC2 boost Switch' $onoff
        amixer cset name='RADC input Mixer MIC2 boost Switch' $onoff
}

function amixer_linein()
{
        onoff=$1
        amixer cset name='LADC input Mixer LINEINL Switch' $onoff
        amixer cset name='RADC input Mixer LINEINR Switch' $onoff
}

function amixer_clear()
{
        amixer cset name='AIF1 AD0L Mixer ADCL Switch' 0
        amixer cset name='AIF1 AD0R Mixer ADCR Switch' 0
        amixer cset name='LADC input Mixer MIC1 boost Switch' 0
        amixer cset name='RADC input Mixer MIC1 boost Switch' 0
        amixer cset name='LADC input Mixer MIC2 boost Switch' 0
        amixer cset name='RADC input Mixer MIC2 boost Switch' 0
        amixer cset name='LADC input Mixer LINEINL Switch' 0
        amixer cset name='RADC input Mixer LINEINR Switch' 0
}

function lineout_mixer_dac_switch()
{
        onoff=$1
        amixer cset name='Left Output Mixer DACL Switch' $onoff
        amixer cset name='Right Output Mixer DACR Switch' $onoff
}

function lineout_mixer_mic1_switch()
{
        onoff=$1
        amixer cset name='Left Output Mixer MIC1Booststage Switch' $onoff
        amixer cset name='Right Output Mixer MIC1Booststage Switch' $onoff
}

function lineout_mixer_mic2_switch()
{
        onoff=$1
        amixer cset name='Left Output Mixer MIC2Booststage Switch' $onoff
        amixer cset name='Right Output Mixer MIC2Booststage Switch' $onoff
}

function lineout_mixer_linein_switch()
{
        onoff=$1
        amixer cset name='Left Output Mixer LINEINL Switch' $onoff
        amixer cset name='Right Output Mixer LINEINR Switch' $onoff
}

function amixer_lineout()
{
        onoff=$1
        amixer cset name='Lineout Switch' $onoff
        amixer cset name='LINEOUTL Mux' $onoff
        amixer cset name='LINEOUTR Mux' $onoff
        #amixer cset name='DACL Mixer AIF1DA0L Switch' $onoff
        #amixer cset name='DACR Mixer AIF1DA0R Switch' $onoff
        #amixer cset name='Left Output Mixer DACL Switch' $onoff
        #amixer cset name='Right Output Mixer DACR Switch' $onoff
}

function clamp_audio_volume()
{
        volume=${1:-10}
        if [ $volume -lt 0 ]; then volume=0; fi
        if [ $volume -gt 10 ]; then volume=10; fi
        echo $volume
}

function dvr_volume_to_hardware()
{
        volume=$(clamp_audio_volume ${1:-10})
        if [ $volume -gt 8 ]; then volume=8; fi
        case $volume in
                0) echo 0 ;;
                1) echo 26 ;;
                2) echo 28 ;;
                3) echo 29 ;;
                4) echo 29 ;;
                5) echo 30 ;;
                6) echo 30 ;;
                7) echo 31 ;;
                *) echo 31 ;;
        esac
}

function live_volume_to_lineout()
{
        volume=$(clamp_audio_volume ${1:-10})
        echo $((($volume * 31 + 5) / 10))
}

function amixer_dvr_volume()
{
        volume=$(clamp_audio_volume ${1:-10})
        hardware_volume=$(dvr_volume_to_hardware $volume)

        if [ $volume -eq 0 ]; then
                dac_volume=0
        elif [ $volume -eq 4 ]; then
                dac_volume=153
        elif [ $volume -eq 6 ]; then
                dac_volume=157
        elif [ $volume -eq 7 ]; then
                dac_volume=158
        else
                dac_volume=$((($hardware_volume * 160 + 15) / 31))
        fi

        amixer cset name='lineout volume' $hardware_volume
        amixer cset name='DAC volume' $dac_volume,$dac_volume
        amixer cset name='AIF1 DAC timeslot 0 volume' $dac_volume,$dac_volume
        amixer cset name='AIF1 DAC timeslot 1 volume' $dac_volume,$dac_volume
}

function amixer_live_volume()
{
        volume=$(clamp_audio_volume ${1:-10})
        lineout_volume=$(live_volume_to_lineout $volume)

        linein_gain=$((($volume * $volume * $volume * 7 + 500) / 1000))
        if [ $volume -gt 0 ] && [ $linein_gain -eq 0 ]; then linein_gain=1; fi

        amixer cset name='lineout volume' $lineout_volume
        amixer cset name='LINEINL/R to L_R output mixer gain' $linein_gain
}

if [ $# -lt 1 ]
then
        echo "params count must at least equal 1"
        exit 1
fi

if [ $1 == "in_mic1" ]
then
        amixer_clear
        amixer_mic1 1
        amixer_adc 1
fi

if [ $1 == "in_mic2" ]
then
        amixer_clear
        amixer_mic2 1
        amixer_adc 1
fi

if [ $1 == "in_linein" ]
then
        amixer_clear
        amixer_linein 1
        amixer_adc 1
fi

if [ $1 == "out_on" ]
then
        amixer_lineout 1
        amixer cset name='lineout volume' 31
fi

if [ $1 == "out_dvr_volume" ]
then
        amixer_dvr_volume ${2:-10}
fi

if [ $1 == "out_live_volume" ]
then
        amixer_live_volume ${2:-10}
fi

if [ $1 == "out_off" ]
then
        amixer_lineout 0
fi

if [ $1 == "out_dac_on" ]
then
        lineout_mixer_dac_switch 1
fi

if [ $1 == "out_dac_off" ]
then
        lineout_mixer_dac_switch 0
fi

if [ $1 == "out_mic1_on" ]
then
        lineout_mixer_mic1_switch 1
fi

if [ $1 == "out_mic1_off" ]
then
        lineout_mixer_mic1_switch 0
fi

if [ $1 == "out_mic2_on" ]
then
        lineout_mixer_mic2_switch 1
fi

if [ $1 == "out_mic2_off" ]
then
        lineout_mixer_mic2_switch 0
fi


if [ $1 == "out_linein_on" ]
then
        lineout_mixer_linein_switch 1
fi

if [ $1 == "out_linein_off" ]
then
        lineout_mixer_linein_switch 0
fi
