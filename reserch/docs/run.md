# 1. Build
./setup_dependencies.sh
meson setup build && meson compile -C build

# 2. Run (Ensure PYTHONPATH is set to pick up the changes!)
export LD_LIBRARY_PATH=$(pwd)/vendor/dist/lib:$LD_LIBRARY_PATH
export PYTHONPATH=$PYTHONPATH:$(pwd)/src
python3 hello_world.py

