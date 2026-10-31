PYTHON ?= python3
HARNESS_CACHE ?=
ifeq ($(origin TOOLS),undefined)
TOOLS := $(shell "$(PYTHON)" fetch_harness.py $(if $(HARNESS_CACHE),--cache "$(HARNESS_CACHE)") --path)
harness:
	"$(PYTHON)" fetch_harness.py $(if $(HARNESS_CACHE),--cache "$(HARNESS_CACHE)") $(if $(filter 1,$(OFFLINE)),--offline)
else
harness:
	@test -f "$(TOOLS)/snapshot_smoke.c" -a -f "$(TOOLS)/run_profiles.py" || { echo 'TOOLS must name a snapshot harness directory'; exit 1; }
endif

.PHONY: harness

.DEFAULT_GOAL := check
OUTPUT ?= build/results
HOST_BUILD := $(CURDIR)/build/host
FIXTURE ?= build/snapshot-history.z64
GPU_FIXTURE ?= build/snapshot-shaded.z64
ifeq ($(OS),Windows_NT)
EXE = .exe
endif

$(FIXTURE): make_fixture.py
	mkdir -p "$(@D)"
	"$(PYTHON)" make_fixture.py "$@"

check: harness $(FIXTURE)
	@test -n "$(TOOLS)" -a -n "$(CORE)" || { echo 'Set CORE to the built core library'; exit 1; }
	$(MAKE) -B -C "$(TOOLS)" BUILD="$(HOST_BUILD)" all
	"$(PYTHON)" "$(TOOLS)/run_profiles.py" --host "$(HOST_BUILD)/snapshot_smoke$(EXE)" --core "$(CORE)" --content "$(abspath $(FIXTURE))" --suite "$(CURDIR)/suite.json" --output "$(abspath $(OUTPUT))"

.PHONY: check

$(GPU_FIXTURE): make_fixture.py
	mkdir -p "$(@D)"
	"$(PYTHON)" make_fixture.py "$@" --shaded

check-gpu: harness $(GPU_FIXTURE)
	@test -n "$(FRONTEND)" -a -n "$(CORE)" || { echo 'Set FRONTEND and CORE to built libraries'; exit 1; }
	$(MAKE) -B -C "$(TOOLS)" BUILD="$(HOST_BUILD)" gpu
	"$(PYTHON)" "$(TOOLS)/run_gpu_profiles.py" --frontend "$(FRONTEND)" --proxy "$(HOST_BUILD)/snapshot_proxy$(if $(EXE),.dll,.so)" --core "$(CORE)" --content "$(abspath $(GPU_FIXTURE))" --suite "$(CURDIR)/gpu-suite.json" --output "$(abspath $(OUTPUT))"

.PHONY: check-gpu
