# Makefile for job_submit/nmpm_custom_resource plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic -lboost_system -lhwdb4c

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common

pkglib_LTLIBRARIES = job_submit_nmpm_custom_resource.la

# Job submit defaults plugin.
job_submit_nmpm_custom_resource_la_SOURCES = job_submit_nmpm_custom_resource.c
job_submit_nmpm_custom_resource_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS)
