import dash_core_components as dcc
import dash_html_components as html

# isort: off
from maindash import spectrum_fig, waterfall_fig, web_interface

# isort: on
from utils import build_sweep_figure

_EMPTY_SWEEP_FIGURE = {
    "data": [{"x": [], "y": [], "type": "scattergl", "mode": "lines", "line": {"color": "#00cc96", "width": 1}}],
    "layout": {
        "template": "plotly_dark",
        "xaxis": {"title": "Frequency (MHz)"},
        "yaxis": {"title": "Power (dBm, approx.)"},
        "margin": {"l": 50, "r": 20, "t": 20, "b": 40},
        "paper_bgcolor": "#000000",
        "plot_bgcolor": "#000000",
    },
}

# Reflects whatever sweep_result was loaded from disk at process startup (see
# WebInterface.__init__) -- a sweep that finished before this process's last
# restart (every sweep ends by restarting the whole service) still shows up
# here without needing to be re-run.
_initial_sweep_figure = build_sweep_figure(web_interface.sweep_result) if web_interface.sweep_result else _EMPTY_SWEEP_FIGURE

layout = html.Div(
    [
        html.Div(
            [
                dcc.Graph(
                    id="spectrum-graph",
                    style={"width": "100%", "height": "45%"},
                    figure=spectrum_fig,  # fig_dummy #spectrum_fig #fig_dummy
                ),
                dcc.Graph(
                    id="waterfall-graph",
                    style={"width": "100%", "height": "65%"},
                    figure=waterfall_fig,  # waterfall fig remains unchanged always due to slow speed to update entire graph #fig_dummy #spectrum_fig #fig_dummy
                ),
            ],
            style={"width": "100%", "height": "80vh"},
        ),
        html.Div(
            [
                html.Div(
                    [
                        html.Button("Sweep 75-7000 MHz (channel 0)", id="btn-start-sweep", className="btn_start"),
                        html.Span(id="sweep-status-text", children="Idle", style={"marginLeft": "1em"}),
                    ]
                ),
                dcc.Graph(
                    id="sweep-graph",
                    style={"width": "100%", "height": "40vh"},
                    figure=_initial_sweep_figure,
                ),
            ],
            className="ctr_toolbar_item",
            style={"width": "100%", "marginTop": "1em"},
        ),
    ]
)
