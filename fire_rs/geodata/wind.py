import os
import subprocess
import itertools

from fire_rs.geodata.basemap import DigitalMap, RasterTile

# DEM tiles are to big to allow computing the wind on the whole tile.
# the following factor is used to split the tile on both axis to build
# smaller DEM tiles to feed windninja
_DEM_TILE_SPLIT = 4

WINDNINJA_CLI_PATH = os.environ['WINDNINJA_CLI_PATH'] \
    if 'WINDNINJA_CLI_PATH' in os.environ else '/home/rbailonr/bin/windninja_cli'
if not os.path.exists(os.path.join(WINDNINJA_CLI_PATH, 'WindNinja_cli')):
    raise FileNotFoundError("$WINDNINJA_CLI_PATH is not defined correctly")


class WindMap(DigitalMap):
    """Wind map associated to an elevation map."""

    def __init__(self, tiles, elevation_map, windninja):
        """Initialise WindMap.

        :param tiles: Sequence of WindTiles
        :param elevation_map: Associated ElevationMap.
        :param scenario: WindNinjaCLI instance with all the necessary arguments set.
        """
        super().__init__(tiles)
        self.elevation_map = elevation_map

        self.scenario = windninja.args
        self.windninja_cli = windninja

        sce_list = []
        if self.scenario['initialization_method'] == 'domainAverageInitialization':
            sce_list.append(str(int(float(self.scenario['input_direction']))))
            sce_list.append(str(int(float(self.scenario['input_speed']))))
            if self.scenario.get('diurnal_winds') is not None and \
               bool(self.scenario['diurnal_winds']) is True:
                sce_list.append('-'.join([self.scenario['month'],
                                          self.scenario['day'],
                                          self.scenario['year']]))
                sce_list.append(''.join([self.scenario['hour'],
                                         self.scenario['minute']]))

            sce_list.append(''.join([str(self.scenario['mesh_resolution']),
                                     self.scenario['units_mesh_resolution']]))

        self.scenario_str = '_'.join(sce_list)  # part of WindNinja output file(s)

    def _load_tile(self, position):
        """Load the tile corresponding to this possition.

        It runs windninja on a subset of the DEM if necessary.
        """
        (x, y) = position
        base_tile = self.elevation_map.tile_of_location(position)

        # find in which subpart of the elevation tile this location is
        xi = int((x - base_tile.x_min) / (base_tile.x_max - base_tile.x_min) * _DEM_TILE_SPLIT)
        yi = int((y - base_tile.y_min) / (base_tile.y_max - base_tile.y_min) * _DEM_TILE_SPLIT)

        # build tile names of this wind scenario and subpart of the DEM tile
        tile_name = os.path.splitext(os.path.split(base_tile.filenames[0])[1])[0] + \
            '[{0}%{2},{1}%{2}]'.format(xi, yi, _DEM_TILE_SPLIT)
        dem_file_name = os.path.join(self.scenario['output_path'], tile_name+'.tif')

        # save smaller DEM tile if it does not exists yet
        if not os.path.exists(dem_file_name):
            dem = base_tile.as_geo_data().split(_DEM_TILE_SPLIT, 1)[xi].split(
                1, _DEM_TILE_SPLIT)[yi]
            assert position in dem
            dem.write_to_file(dem_file_name)

        windfile_paths = [os.path.join(self.scenario['output_path'],
                                       '_'.join([tile_name,
                                                 self.scenario_str,
                                                 'vel.asc'])),
                          os.path.join(self.scenario['output_path'],
                                       '_'.join([tile_name,
                                                 self.scenario_str,
                                                 'ang.asc']))]

        if not(os.path.exists(windfile_paths[0]) and os.path.exists(windfile_paths[1])):
            self._run_windninja(dem_file_name)

        return WindTile(windfile_paths)

    def _run_windninja(self, elevation_file: str):
        self.windninja_cli.set_elevation_file(elevation_file)
        completed = self.windninja_cli.run()
        if completed.returncode != 0:
            raise RuntimeError("Error during execution! WindNinja returned {}.".format(
                completed.returncode))

    def get_value(self, position):
        """Get the value corresponding to a position."""
        if position not in self.elevation_map:
            raise KeyError("Location out of elevation map bounds")

        if position not in self:
            # We have to load a map for this position
            tile = self._load_tile(position)
            self.add_tile(tile)
            return tile[position]
        return super().get_value(position)

    def get_values(self, positions_intervals):
        ((x_min, x_max), (y_min, y_max)) = positions_intervals
        assert all([p in self.elevation_map for p in itertools.product((x_min,x_max), (y_min, y_max))]),\
               'The requested rectangle is not contained in the known DEM tiles'
        sample_tile = self.elevation_map.tile_of_location((x_min, y_min))
        subtile_width = (sample_tile.x_max - sample_tile.x_min + sample_tile.x_delta) / _DEM_TILE_SPLIT

        # round x/y to cell centers
        (x_min, y_min) = self.elevation_map.tile_of_location((x_min, y_min)).nearest_projected_point((x_min, y_min))
        (x_max, y_max) = self.elevation_map.tile_of_location((x_max, y_max)).nearest_projected_point((x_max, y_max))

        # sample xs and ys so we have one in each subcell
        xs = list(range(int(x_min), int(x_max), int(subtile_width))) + [int(x_max)]
        ys = list(range(int(y_min), int(y_max), int(subtile_width))) + [int(y_max)]

        # force loading all subtiles
        for pos in itertools.product(xs, ys):
            self.get_value(pos)

        # now that all tiles are loaded, rely on the generic method
        return super().get_values(positions_intervals)

    def get_wind(self, position):
        """Get the wind vector of a position."""
        return self.get_value(position)


class WindTile(RasterTile):

    def __init__(self, windfile_paths):
        super().__init__(windfile_paths, [('wind_velocity', 'float32'), ('wind_angle', 'float32')])

    def get_wind(self, location):
        return self.get_value(location)


class WindNinjaCLI():

    def __init__(self, path=WINDNINJA_CLI_PATH, cli_arguments=None):
        self.windninja_path = path
        self.args = {}  # dict(arg, value)
        num_threads = len(os.sched_getaffinity(0)) if "sched_getaffinity" in dir(os) else 2,
        self.add_arguments(num_threads=num_threads,
                           output_speed_units='mps',
                           mesh_resolution=25,  # ¡! Conflicts with mesh_choice
                           units_mesh_resolution='m',
                           write_ascii_output='true')
        if cli_arguments is not None:
            self.add_arguments(**cli_arguments)

    def args_str(self):
        return " ".join(["=".join(("".join(
            ("--", key)), value)) for key, value in self.args.items()])

    def args_list(self):
        return ["=".join(("".join(("--", key)), value)) for key, value in self.args.items()]

    @property
    def elevation_file(self):
        return self.args.get('elevation_file')

    @elevation_file.setter
    def elevation_file(self, path):
        self.args['elevation_file'] = path

    def set_elevation_file(self, file_path):
        self.elevation_file = file_path

    @property
    def output_path(self):
        return self.args.get('output_path')

    @output_path.setter
    def output_path(self, output_folder):
        """Output path.

        It must exist already. If not, WindNinja won't create it and will
        write to the default location"""
        self.args['output_path'] = output_folder

    def set_output_path(self, output_folder):
        """Set output path.

        It must exist already. If not, WindNinja won't create it and will
        write to the default location"""
        self.output_path = output_folder

    def add_arguments(self, **kwargs):
        for key, value in kwargs.items():
            self.args[str(key)] = str(value)

    def run(self):
        """Run WindNinja.

        :return: subprocess.CompletedProcess instance.
        """
        return self.run_blocking()

    def run_blocking(self):
        """Run WindNinja.

        This function blocks the execution until the process is finished.

        :return: subprocess.CompletedProcess instance.
        """
        arguments = self.args_list()
        cli = os.path.join(self.windninja_path, "WindNinja_cli")
        # Working directory must be the one in which WindNinja_cli is located
        # because it searchs locally the date_time_zonespec.csv.
        # In my opinion this is a bug in WindNinja_cli.
        completed = subprocess.run((cli, *arguments), cwd=self.windninja_path)
        return completed

    @staticmethod
    def domain_average_args(input_speed, input_direction, vegetation='trees'):
        """Generate a set of arguments for WindNinja for a domain average input.

        :param input_speed: input wind speed in m/s
        :param input_direction: input wind direction in degrees
        :param vegetation: vegetation type (grass, brush or trees)
        :return: arguments for WindNinja
        :rtype: dict
        """
        args = {'initialization_method': 'domainAverageInitialization',
                'input_speed': input_speed,
                'input_speed_units': 'mps',
                'input_direction': input_direction,
                'input_wind_height': '10.0',
                'units_input_wind_height': 'm',
                'output_wind_height': '10.0',
                'units_output_wind_height': 'm',
                'vegetation': vegetation}
        return args

    @staticmethod
    def diurnal_winds_args(uni_air_temp, uni_cloud_cover, date,
                           air_temp_units='C', cloud_cover_units='fraction'):
        """Generate a set of arguments to use diurnal winds in simulation.

        :param uni_air_temp: Surface air temperature.
        :param uni_cloud_cover: Cloud cover.
        :param date: datetime object defining a date, a time of the day and
            a timezone. For instance: 'Europe/Paris' or pytz.timezone('Europe/Paris')
        :param air_temp_units: Surface air temperature units {K | C | R | F}.
        :param cloud_cover_units: Cloud cover units {fraction | percent | canopy_category}.
        """
        args = {'diurnal_winds': 'true',
                'uni_air_temp': uni_air_temp,
                'air_temp_units': str(air_temp_units),
                'uni_cloud_cover': uni_cloud_cover,
                'cloud_cover_units': str(cloud_cover_units),
                'year': str(date.year),
                'month': str(date.month),
                'day': str(date.day),
                'hour': str(date.hour),
                'minute': str(date.minute),
                'time_zone': str(date.tzinfo)}
        return args

    @staticmethod
    def use_momentum_solver_args(number_of_iterations=300):
        """Generate a set of arguments for activating the momentum solver in WindNinja."""
        args = {'momentum_flag': 'true',
                'number_of_iterations': number_of_iterations}
        return args

    @staticmethod
    def output_type_args(write_ascii_output=True,
                         write_shapefile_output=False,
                         write_goog_output=False,
                         write_farsite_atm=False,
                         write_pdf_output=False,
                         write_vtk_output=False):

        args = {'write_ascii_output': str(write_ascii_output).lower(),
                'write_shapefile_output': str(write_shapefile_output).lower(),
                'write_goog_output': str(write_goog_output).lower(),
                'write_farsite_atm': str(write_farsite_atm).lower(),
                'write_pdf_output': str(write_pdf_output).lower(),
                'write_vtk_output': str(write_vtk_output).lower()}
        return args

    @staticmethod
    def weather_model_output_type_args(write_wx_model_ascii_output=True,
                                       write_wx_model_shapefile_output=False,
                                       write_wx_model_goog_output=False):
        args = {'write_wx_model_ascii_output': str(write_wx_model_ascii_output).lower(),
                'write_wx_model_shapefile_output': str(write_wx_model_shapefile_output).lower(),
                'write_wx_model_goog_output': str(write_wx_model_goog_output).lower()}
        return args
