# Makefile for qrcon

obj-$(CONFIG_QRCON) := qrcon_mod.o
qrcon_mod-objs := qrcon.o qr_generator.o
