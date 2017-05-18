import unittest
import fire_rs.firemodel.propagation as propagation


class TestPropagation(unittest.TestCase):

    def test_propagate(self):
        env = propagation.Environment([[475060.0, 476060.0], [6200074.0, 6201074.0]], wind_speed=4.11, wind_dir=0)
        prop = propagation.propagate(env, 0, 0)
        ignitions = prop.slice(['ignition'])
        ignitions.write_to_file('/tmp/igni.tif')
        env.raster.write_to_separate_files('/tmp/env__%s.tif')
