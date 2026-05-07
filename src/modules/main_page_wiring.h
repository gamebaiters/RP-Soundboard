// Wires MainPage's child widgets to ConfigModel + Sampler. Kept out of
// main_page.cpp so the layout stays free of cross-module logic.

#pragma once

class MainPage;
class ConfigModel;
class Sampler;

namespace MainPageWiring {

void wire(MainPage *page, ConfigModel *model, Sampler *sampler);

}
