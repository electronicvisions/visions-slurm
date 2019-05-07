# Makefile for job_submit/hagen_daas plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic -lboost_system -ltoml

AM_CPPFLAGS = -I$(top_srcdir)

pkglib_LTLIBRARIES = job_submit_hagen_daas.la

# Job submit defaults plugin.
job_submit_hagen_daas_la_SOURCES = job_submit_hagen_daas.c hagen_daas_config.c
job_submit_hagen_daas_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS)
