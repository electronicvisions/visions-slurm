# Makefile for job_submit/cerberus plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic -lboost_system -ltoml

AM_CPPFLAGS = -I$(top_srcdir)

pkglib_LTLIBRARIES = job_submit_cerberus.la

# Job submit defaults plugin.
job_submit_cerberus_la_SOURCES = job_submit_cerberus.c cerberus_config.c
job_submit_cerberus_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS)
