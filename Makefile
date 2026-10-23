BUILD ?= build
JOBS ?= 4

.PHONY: all build test asan valgrind web api run demo bench docker clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j $(JOBS)

test: build
	cd $(BUILD) && ctest --output-on-failure

asan:
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DFAVO_SANITIZE=ON
	cmake --build build-asan -j $(JOBS)
	cd build-asan && ctest --output-on-failure

valgrind: build
	valgrind --error-exitcode=1 --leak-check=full $(BUILD)/test_basic
	valgrind --error-exitcode=1 $(BUILD)/test_policies

web:
	cd web && npm ci && npm run build

api: build
	cd api && pip install -r requirements-dev.txt && FAVO_LIBRARY=../$(BUILD)/libfavo.so python -m pytest

run: build web
	cd api && FAVO_LIBRARY=../$(BUILD)/libfavo.so python -m favo_api

demo: build
	$(BUILD)/favo demo --policy best

bench: build
	$(BUILD)/favo bench --ops 500000

docker:
	docker compose up -d --build

clean:
	rm -rf $(BUILD) build-asan web/dist
