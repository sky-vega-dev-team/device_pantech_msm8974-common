#!/vendor/bin/sh
#
# VoLTE of the Korean carriers through phh IMS.
#
# The modem profile of the ims APN asks for the P-CSCF in the PCO (and, on
# KT/SKT, marks the PDN as IMS signalling). The IM CN flag crashes the LG U+
# modem (ps_utils.c:352), so it is never set there, nor left on another profile.
# Then the P-CSCF of the IMS PDN is stored for phh IMS.

QMI=/vendor/bin/qmi_pcscf

case "$(getprop gsm.sim.operator.numeric)" in
    45008|45005) $QMI setup 1 1 ;;  # KT, SKT
    45006)       $QMI setup 1 0 ;;  # LG U+
    *)           exit 0 ;;
esac

# The IMS PDN comes up after the network registration
i=0
while [ $i -lt 60 ]; do
    $QMI cache >/dev/null 2>&1 && exit 0
    sleep 5
    i=$((i + 1))
done
exit 0
