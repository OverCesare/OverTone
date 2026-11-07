# macOS: Apple Command Line Tools are the only required build tools.
CC := xcrun clang
export SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
BUILD ?= build$(if $(filter 1,$(SANITIZE)),/sanitize,)
INSTALL_DIR ?= /Applications
APP := $(INSTALL_DIR)/OverTone.app
BINARY := $(BUILD)/overtone
SIGN_IDENTITY ?= -
PACKAGE ?= $(BUILD)/OverTone-local.zip
CPPFLAGS := -Isrc -isystem third_party/yyjson -DYYJSON_DISABLE_FILE=1 -DYYJSON_DISABLE_NON_STANDARD=1 -DYYJSON_READER_DEPTH_LIMIT=64 -DYYJSON_WRITER_DEPTH_LIMIT=64
OPTFLAGS := -O3 -flto=thin
ifeq ($(SANITIZE),1)
OPTFLAGS := -O1
endif
MACOS_MIN := $(shell /usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' src/macos/Info.plist)
CFLAGS := -std=c17 $(OPTFLAGS) -g -arch arm64 -Werror -Wunguarded-availability -MMD -MP -mmacosx-version-min=$(MACOS_MIN)
CWARN := -Weverything -Wno-padded -Wno-declaration-after-statement -Wno-c++-compat
OBJCWARN := -Wall -Wextra -Wconversion -Wshadow -Wformat=2
LDFLAGS := -mmacosx-version-min=$(MACOS_MIN) -Wl,-dead_strip -lsqlite3 -lpthread $(OPTFLAGS)
ifneq ($(filter -flto%,$(CFLAGS)),)
LTO_DEBUG = -Wl,-object_path_lto,$@.lto.o
endif
ifeq ($(SANITIZE),1)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif
SOURCES := $(wildcard src/*.c) src/macos/engine.c src/macos/diagnostics.c src/macos/package.c src/macos/process.c src/macos/pull.c
OBJECTS := $(SOURCES:%.c=$(BUILD)/%.o) $(BUILD)/vendor/yyjson.o
APP_SOURCES := $(wildcard src/macos/*.m)
APP_OBJECTS := $(APP_SOURCES:%.m=$(BUILD)/%.o)
.PHONY: all install package run check fuzz analyze
all: $(BINARY)
$(OBJECTS) $(APP_OBJECTS) $(BUILD)/tests/checks.o $(BUILD)/tests/fuzz.o: src/macos/Info.plist
$(BUILD)/%.o: %.c Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CWARN) -c "$<" -o "$@"
$(BUILD)/%.o: %.m Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OBJCWARN) -fobjc-arc -c "$<" -o "$@"
$(BUILD)/vendor/yyjson.o: third_party/yyjson/yyjson.c Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -c "$<" -o "$@"
$(BINARY): $(OBJECTS) $(APP_OBJECTS)
	@mkdir -p "$(@D)"
	$(CC) $^ $(LDFLAGS) $(LTO_DEBUG) -framework AppKit -framework QuartzCore -framework WebKit -framework Metal -framework NaturalLanguage -framework ScreenCaptureKit -framework CoreMedia -framework CoreVideo -framework Security -o "$@"
install: all
	@if /usr/bin/pgrep -x overtone >/dev/null; then echo "Close OverTone before installing." >&2; exit 1; fi
	@mkdir -p "$(INSTALL_DIR)"
	@if [ -L "$(APP)" ]; then echo "Refusing to replace an application symlink: $(APP)" >&2; exit 1; fi
	@set -eu; work=$$(mktemp -d "$(INSTALL_DIR)/.overtone-install.XXXXXX"); \
	trap 'if [ ! -e "$(APP)" ] && [ -d "$$work/previous" ]; then mv "$$work/previous" "$(APP)" || { echo "Previous app retained at $$work/previous" >&2; exit 1; }; fi; rm -rf "$$work"' EXIT; \
	trap 'exit 1' HUP INT TERM; \
	stage="$$work/OverTone.app"; \
	mkdir -p "$$stage/Contents/MacOS" "$$stage/Contents/Resources"; \
	cp "$(BINARY)" "$$stage/Contents/MacOS/overtone"; \
	cp src/macos/Info.plist "$$stage/Contents/Info.plist"; \
	/usr/libexec/PlistBuddy -c 'Set :LSMinimumSystemVersion $(MACOS_MIN)' "$$stage/Contents/Info.plist"; \
	cp src/macos/OverTone.icns src/macos/Assets.car "$$stage/Contents/Resources/"; \
	cp -R ui "$$stage/Contents/Resources/ui"; \
	cp LICENSE THIRD_PARTY_NOTICES.md "$$stage/Contents/Resources/"; \
	cp third_party/yyjson/LICENSE "$$stage/Contents/Resources/yyjson-LICENSE"; \
	flags='--options runtime'; \
	if [ "$(SANITIZE)" = 1 ]; then flags=''; else xcrun strip -S "$$stage/Contents/MacOS/overtone"; fi; \
	if [ "$(SIGN_IDENTITY)" != - ]; then flags="$$flags --timestamp"; fi; \
	codesign --force --sign "$(SIGN_IDENTITY)" $$flags "$$stage"; \
	codesign --verify --strict --verbose=2 "$$stage"; \
	if [ -e "$(APP)" ]; then mv "$(APP)" "$$work/previous"; fi; \
	mv "$$stage" "$(APP)"
ifeq ($(SANITIZE),1)
package:
	@echo "Do not package an instrumented build." >&2; exit 1
else
package: install
	@mkdir -p "$$(dirname "$(PACKAGE)")"
	@set -eu; archive=$$(mktemp "$(PACKAGE).XXXXXX"); \
	trap 'rm -f "$$archive"' EXIT; trap 'exit 1' HUP INT TERM; \
	ditto -c -k --keepParent --norsrc "$(APP)" "$$archive"; \
	mv "$$archive" "$(PACKAGE)"
	@cd "$$(dirname "$(PACKAGE)")" && \
	shasum -a 256 -- "$$(basename "$(PACKAGE)")" > "$$(basename "$(PACKAGE)").sha256"
endif
$(BUILD)/checks: $(OBJECTS) $(BUILD)/tests/checks.o
	$(CC) $^ $(LDFLAGS) $(LTO_DEBUG) -o "$@"
check: $(BUILD)/checks
	"$(BUILD)/checks"
analyze:
	@set -eu; for source in $(SOURCES); do \
	$(CC) $(CPPFLAGS) $(filter-out -MMD -MP -flto%,$(CFLAGS)) $(CWARN) --analyze \
	-Xanalyzer -analyzer-output=text -o /dev/null "$$source"; done
	@set -eu; for source in $(APP_SOURCES); do \
	$(CC) $(CPPFLAGS) $(filter-out -MMD -MP -flto%,$(CFLAGS)) $(OBJCWARN) -fobjc-arc --analyze \
	-Xanalyzer -analyzer-output=text -o /dev/null "$$source"; done
$(BUILD)/fuzz: $(OBJECTS) $(BUILD)/tests/fuzz.o
	$(CC) $^ $(LDFLAGS) $(LTO_DEBUG) -o "$@"
fuzz: $(BUILD)/fuzz
	"$(BUILD)/fuzz"
run: install
	open "$(APP)"
-include $(OBJECTS:.o=.d) $(APP_OBJECTS:.o=.d) $(BUILD)/tests/checks.d $(BUILD)/tests/fuzz.d
