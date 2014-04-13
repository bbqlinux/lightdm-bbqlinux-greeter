import os
import commands
import sys

# configure lightdm
lightdmconfig = open("/etc/lightdm/lightdm.conf", "r")
newlightdmconfig = open("/etc/lightdm/lightdm.conf.new", "w")

for line in lightdmconfig:
    line = line.rstrip("\r\n")
    if(line.startswith("greeter-session=lightdm-bbqlinux-greeter")):
        newlightdmconfig.write("greeter-session=lightdm-gtk-greeter\n")
    else:
        newlightdmconfig.write("%s\n" % line)

lightdmconfig.close()
newlightdmconfig.close()

os.system("rm /etc/lightdm/lightdm.conf")
os.system("mv /etc/lightdm/lightdm.conf.new /etc/lightdm/lightdm.conf")
