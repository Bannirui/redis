# Top level makefile, the real shit is at src/Makefile

# 执行make的时候找到的第一个target是default 该target依赖一个dependency为all
# 但是该Makefile中没有再定义为all的target 即make找不到叫all的target
# 在这种情况下make会执行.DEFAULT
default: all

# 因为make的执行是从default作为入口下来的
# 因此$@指代的是all
# 也就是说要执行的shell是cd src && make all
.DEFAULT:
	cd src && $(MAKE) $@

install:
	cd src && $(MAKE) $@

.PHONY: install
