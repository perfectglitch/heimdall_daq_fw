# isort: off
from maindash import app, web_interface

# isort: on

from dash_devices.dependencies import Input
from utils import start_wideband_sweep


@app.callback_shared(None, [Input(component_id="btn-start-sweep", component_property="n_clicks")])
def start_sweep_btn(n_clicks):
    if not n_clicks:
        return
    web_interface.logger.info("Wideband sweep button pushed")
    start_wideband_sweep(app, web_interface)
