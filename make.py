#!/usr/bin/env python
from fabricate import *
import sys, os
ROOT = os.path.realpath(os.path.dirname(sys.argv[0]))
sys.path.append(ROOT + '/config')
os.environ['PYTHONPATH'] = ':'.join([(ROOT + '/config'), (ROOT + '/goo')])

import config
cfg = config.openconfig()

SDK = '/var/sdk'
BIN = '/Developer/Platforms/iPhoneOS.platform/Developer/usr/bin'
GCC_BIN = BIN + '/gcc-4.2'
GCC_BASE = [GCC_BIN, '-Werror', '-Os', '-Wimplicit', '-isysroot', SDK, '-F'+SDK+'/System/Library/Frameworks', '-F'+SDK+'/System/Library/PrivateFrameworks', '-I', ROOT, '-fno-blocks']
GCC = [GCC_BASE, '-arch', cfg['arch']]
GCC_UNIVERSAL = [GCC_BASE, '-arch', 'armv6', '-arch', 'armv7']
GCC_NATIVE = 'gcc'
GCC_FLAGS = ['-std=gnu99']
HEADERS = ROOT + '/headers'

def goto(dir):
    os.chdir(os.path.join(ROOT, dir))

def chext(f, ext):
    return f[:f.find('.')] + ext

def F(*frameworks):
    ret = []
    for framework in frameworks:
        ret.append('-framework')
        ret.append(framework)
    return ret

def compile_to_bin(filename):
    # requires machdump
    ofile = chext(filename, '.o')
    binfile = chext(filename, '.bin')
    run(GCC, '-mthumb', '-c', '-o', ofile, filename)
    run(ROOT + '/machdump/machdump', ofile, binfile)
    if os.path.exists(ofile): os.unlink(ofile)

def pmap():
    goto('star/pmap')
    for x in ['pmap2', 'pmaparb', 'shelltester']:
        run(GCC_UNIVERSAL, '-o', x, x + '.c', '-I', headers, '-std=gnu99', F('IOKit', 'CoreFoundation', 'IOSurface'))



def machdump():
    goto('machdump')
    run(GCC_NATIVE, '-o', 'machdump', 'machdump.c')

def install():
    goto('install')
    files = ['install.o', 'copier.o']
    for o in files:
        run(GCC_UNIVERSAL, '-I', HEADERS, '-std=gnu99', '-c', '-o', o, chext(o, '.c'))
    run(GCC_UNIVERSAL, '-dynamiclib', '-o', 'install.dylib', files, F('CoreFoundation', 'GraphicsServices'), '-L.', '-ltar', '-llzma')
    run('python', 'wad.py', 'install.dylib', 'Cydia-whatever.txz')

def installui():
    goto('installui')
    if not os.path.exists('dumpedUIKit'):
        run('./mkUIKit.sh')
        
    files = ['dddata.o', 'installui.o']
    for o in files:
        run(GCC_UNIVERSAL, '-I', HEADERS, '-I', '.', '-std=gnu99', '-c', '-o', o, chext(o, '.m'))
    run(GCC_UNIVERSAL, '-dynamiclib', '-o', 'installui.dylib', files, F('Foundation', 'UIKit', 'IOKit', 'CoreGraphics'), '-lz')

def sandbox():
    machdump()
    goto('sandbox')
    compile_to_bin('sandbox-mac-replace.s')

def goo():
    pass

def goo_iosurface():
    sandbox()
    installui()
    goo()
    machdump()
    goto('goo/iosurface')
    compile_to_bin('goop.s')
    run('python', 'zero.py', '--boot')
    run('python', '../one.py', 'stage2boot.txt')
    run('python', 'zero.py', '--initial')

def cff():
    goo_iosurface()
    goto('cff')
    run('python', 'mkpdf.py')
    run('python', 'outcff.py')

def star():
    install()
    cff()

def clean():
    autoclean()

main()