JACK MIDI TO OSC
================

A small utility that generates [OSC](http://opensoundcontrol.org) triggered
by [JACK](http://jackaudio.org/) MIDI events.

Usage
-----

See the included manual page as well as example configuration files.

Install
-------

Compiling this application requires the JACK, liblo, gnu-make and a c-compiler.

```bash
  git clone git://github.com/x42/jackmidi2osc.git
  cd jackmidi2osc
  make
  # optionally install
  sudo make install PREFIX=/usr
  
  # test run
  ./jackmidi2osc -v -c cfg/example.cfg
  # check for messages via 
  # oscdump 5849
```

Note to packagers: The Makefile honors `PREFIX` and `DESTDIR` variables as well
common make variables. `CFLAGS` defaults to `-Wall -O3 -g`.

```bash
make CFLAGS="-Werror -O3"
make install DESTDIR=/tmp/ PREFIX=/usr
```

See Also
--------

[osc2midi](http://sourceforge.net/projects/osc2midi/)
