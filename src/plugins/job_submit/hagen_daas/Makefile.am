# Makefile for job_submit/nmpm_custom_resource plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic -lboost_system

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common

pkglib_LTLIBRARIES = hagen_daas.la

# Job submit defaults plugin.
hagen_daas_la_SOURCES = hagen_daas.c
hagen_daas_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS)
