import dash_devices as dash
from kraken_web_interface import WebInterface
from kraken_web_spectrum import init_spectrum_fig
from utils import start_health_monitor
from variables import fig_layout, trace_colors
from waterfall import init_waterfall

# app = dash.Dash(__name__, suppress_callback_exceptions=True,
# compress=True, update_title="") # cannot use update_title with
# dash_devices
app = dash.Dash(__name__, suppress_callback_exceptions=True)
app.title = "KrakenSDR DoA"
app.config.suppress_callback_exceptions = True

# app_log = logger.getLogger('werkzeug')
# app_log.setLevel(settings.logging_level*10)
# app_log.setLevel(30) # TODO: Only during dev time

#############################################
#          Prepare Dash application         #
############################################
web_interface = WebInterface()

# Started unconditionally here (not on client-connect like fetch_dsp_data/
# fetch_gps_data) -- a health check that only runs while a browser happens
# to be open isn't a health check.
start_health_monitor(app, web_interface)

#############################################
#       Prepare component dependencies      #
#############################################
spectrum_fig = init_spectrum_fig(web_interface, fig_layout, trace_colors)
waterfall_fig = init_waterfall(web_interface)
