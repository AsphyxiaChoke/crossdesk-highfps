#include "crossdesk_ui.h"
#include "ui/model_update.h"

#include <slint-platform.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
// Match the retained software buffer used by the Harmony desktop adapter.
// A full window snapshot would conceal missing damage/redraw notifications.
class TestWindow : public slint::platform::WindowAdapter {
 public:
  slint::platform::SoftwareRenderer software{
      slint::platform::SoftwareRenderer::RepaintBufferType::ReusedBuffer};
  std::vector<slint::Rgb8Pixel> pixels = std::vector<slint::Rgb8Pixel>(1280 * 720);
  bool dirty = true;
  slint::PhysicalSize size() override { return slint::PhysicalSize({1280, 720}); }
  slint::platform::AbstractRenderer& renderer() override { return software; }
  void request_redraw() override { dirty = true; }
  void Render() {
    if (!dirty && !window().has_active_animations()) return;
    dirty = false;
    software.render(pixels, 1280);
  }
};

class TestPlatform : public slint::platform::Platform {
 public:
  TestWindow* adapter = nullptr;
  std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override {
    auto result = std::make_unique<TestWindow>();
    adapter = result.get();
    return result;
  }
};
}  // namespace

int main() {
  auto platform = std::make_unique<TestPlatform>();
  auto* backend = platform.get();
  slint::platform::set_platform(std::move(platform));
  auto stream = crossdesk::ui::StreamWindow::create();
  auto& adapter = *backend->adapter;
  stream->window().dispatch_resize_event(slint::LogicalSize({1280, 720}));
  stream->show();
  stream->set_secure_attention_supported(false);
  stream->set_privacy_visible(true);
  auto rows = std::make_shared<slint::VectorModel<crossdesk::ui::NetworkStatsRow>>();
  stream->set_stats_rows(rows);
  std::vector<crossdesk::ui::NetworkStatsRow> stats(4);
  unsigned samples = 0;
  bool stats_visible = false;
  stream->on_toggle_stats([&] { stats_visible = !stats_visible; });
  // Synchronize inside a Slint timer, exactly as GuiApplication does. Updating
  // before update_timers_and_animations would use the previous animation tick.
  slint::Timer sync;
  sync.start(slint::TimerMode::Repeated, std::chrono::milliseconds(16), [&] {
    stream->set_stats_visible(stats_visible);
    // Keep values changing while the panel animates, as during a live session.
    stats[0].inbound = slint::SharedString(std::to_string(++samples) + " kbps");
    crossdesk::ui::UpdateModelRows(*rows, stats);
  });
  const auto tick = [&] {
    slint::platform::update_timers_and_animations();
    adapter.Render();
  };
  const auto settle = [&](int frames = 12) {
    for (int i = 0; i < frames; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      tick();
    }
  };
  settle();
  const float top = stream->get_video_area_top();
  const auto is_dark_surface = [](slint::Rgb8Pixel p) {
    return p.r >= 12 && p.r <= 80 && p.g >= 14 && p.g <= 90 && p.b >= 20 && p.b <= 110;
  };
  const auto click_pill = [&] {
    const slint::LogicalPosition pill({
        stream->get_control_pill_x() + stream->get_control_pill_width() / 2,
        top + stream->get_control_pill_y() + stream->get_control_pill_height() / 2});
    stream->window().dispatch_pointer_move_event(pill);
    stream->window().dispatch_pointer_press_event(pill, slint::PointerEventButton::Left);
    stream->window().dispatch_pointer_release_event(pill, slint::PointerEventButton::Left);
    settle();
  };
  // The initial control is only the UU floating capsule. Expand the Control Center first.
  click_pill();
  for (bool dock_right : {false, true}) {
    if (dock_right) {
      stream->invoke_begin_control_drag(64, 25);
      stream->invoke_move_control_drag(1220, 25);
      stream->invoke_end_control_drag();
      settle();
      click_pill();
    }
    const int panel_x = int(stream->get_control_panel_x());
    const int panel_y = int(stream->get_control_panel_y_pos());
    if (!is_dark_surface(adapter.pixels[(int(top) + panel_y + 30) * 1280 + panel_x + 4])) {
      std::cerr << "UU Control Center panel did not render dark surface; dock_right=" << dock_right << '\n';
      return 1;
    }
    const slint::LogicalPosition button(
        {float(panel_x + 308), top + stream->get_quick_grid_y() + 81});
    stream->window().dispatch_pointer_move_event(button);
    settle();
    for (bool expanded : {true, false, true, false}) {
      stream->window().dispatch_pointer_press_event(button, slint::PointerEventButton::Left);
      stream->window().dispatch_pointer_release_event(button, slint::PointerEventButton::Left);
      settle(); // No pointer movement: panel must appear/clear on its own.
      if (stats_visible != expanded) {
        std::cerr << "Network status tile did not toggle the panel; dock_right="
                  << dock_right << '\n';
        return 1;
      }
      const int stats_x = int(stream->get_stats_card_x());
      const int stats_y = int(stream->get_stats_card_y());
      for (int y : {stats_y + 25, stats_y + 180}) {
        const auto pixel = adapter.pixels[(int(top) + y) * 1280 + stats_x + 4];
        const bool painted = is_dark_surface(pixel);
        const bool cleared = pixel.r == 0 && pixel.g == 0 && pixel.b == 0;
        if (expanded ? !painted : !cleared) {
          std::cerr << "Stats must fully " << (expanded ? "expand" : "collapse")
                    << " without pointer movement; dock_right=" << dock_right
                    << ", y=" << y << ", pixel=" << int(pixel.r) << ','
                    << int(pixel.g) << ',' << int(pixel.b) << '\n';
          return 1;
        }
        if (stream->invoke_is_local_control_area(stats_x + 4, y) != expanded) {
          std::cerr << "Stats input area must match the rendered panel\n";
          return 1;
        }
      }
    }
    // Collapse Control Center and confirm retained-buffer pixels clear cleanly.
    click_pill();
    const auto cleared_panel = adapter.pixels[(int(top) + panel_y + 30) * 1280 + panel_x + 4];
    if (cleared_panel.r != 0 || cleared_panel.g != 0 || cleared_panel.b != 0) {
      std::cerr << "Collapsing UU Control Center must clear panel pixels; dock_right=" << dock_right << '\n';
      return 1;
    }
  }
  std::cout << "Stream stats retained-buffer rendering passed.\n";
}
