#!/usr/bin/env python

def depends(ctx):
    pass

def options(opt):
    pass

def configure(cfg):
    pass

def build(bld):
    bld(target          = 'visions-slurm_inc',
        export_includes = '.'
    )
