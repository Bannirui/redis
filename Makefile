# Top level makefile, the real shit is at src/Makefile

# 相当于 cd src && make all
default: all

.DEFAULT:
	cd src && $(MAKE) $@

install:
	cd src && $(MAKE) $@

.PHONY: install
