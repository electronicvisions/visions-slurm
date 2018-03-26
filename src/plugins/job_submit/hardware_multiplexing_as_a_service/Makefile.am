# Makefile for job_submit/nmpm_custom_resource plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic -lboost_system

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common

pkglib_LTLIBRARIES = hardware_multiplexing_as_a_service.la

# Job submit defaults plugin.
hardware_multiplexing_as_a_service_la_SOURCES = hardware_multiplexing_as_a_service.c
hardware_multiplexing_as_a_service_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS)
