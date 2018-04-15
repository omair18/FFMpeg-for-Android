from argparse import ArgumentParser
import os

__author__ = 'Ilja Kosynkin'

parser = ArgumentParser()
parser.add_argument("-p", "--prefix", dest="prefix",
                    help="Prefix to directory that contains builded module")

args = parser.parse_args()

prefix = args.prefix
if prefix is None:
    prefix = ""
elif prefix[-1] != '/':
    prefix += '/'

out = open(prefix + "Android.mk", 'w+')
out.write("LOCAL_PATH:= $(call my-dir)\n\n")

for path in os.listdir(prefix + "/lib"):
    splitted = path.split('-')
    if len(splitted) == 2:
        out.write("include $(CLEAR_VARS)\n")
        out.write("LOCAL_MODULE:= %s\n" % splitted[0])
        out.write("LOCAL_SRC_FILES:= lib/%s\n" % path)
        out.write("LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/include\n")
        out.write("include $(PREBUILT_SHARED_LIBRARY)\n\n")

out.close()
