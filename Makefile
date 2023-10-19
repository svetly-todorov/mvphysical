CXXFLAGS =  -g -std=c++17 -D_GNU_SOURCE -Wall
CXXFLAGS += -Wformat-truncation=2 -Werror=class-memaccess -Werror=return-type -Werror=int-in-bool-context
CXXFLAGS += -Werror=parentheses -Werror=overflow

TARGETS = mvphysical

all: $(TARGETS)

clean:
	rm -f $(TARGETS)

mvphysical: mvphysical.cc
	g++  $(CXXFLAGS) -o $@ $< -lnuma
