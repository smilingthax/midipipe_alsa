SOURCES=main.c
EXEC=midipipe

CPPFLAGS=-O3 -funroll-all-loops -finline-functions -Wall
#CPPFLAGS+=-DMIDILIB_ALSA
LIBS=-lasound

OBJECTS=$(patsubst %.c,$(PREFIX)%$(SUFFIX).o,\
        $(patsubst %.cpp,$(PREFIX)%$(SUFFIX).o,\
$(SOURCES)))
DEPENDS=$(patsubst %.c,$(PREFIX)%$(SUFFIX).d,\
        $(patsubst %.cpp,$(PREFIX)%$(SUFFIX).d,\
        $(filter-out %.o,""\
$(SOURCES))))

all: $(EXEC)
ifneq "$(MAKECMDGOALS)" "clean"
  -include $(DEPENDS)
endif

clean:
	rm -f $(EXEC) $(OBJECTS) $(DEPENDS)

%.d: %.c
	@$(CC) $(CPPFLAGS) -MM -MT"$@" -MT"$*.o" -o $@ $<  2> /dev/null

%.d: %.cpp
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MM -MT"$@" -MT"$*.o" -o $@ $<  2> /dev/null

$(EXEC): $(OBJECTS)
	$(CC) -o $@ $^ $(LIBS)
