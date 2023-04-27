/*
 * \brief  Sculpt system manager for a phone
 * \author Norman Feske
 * \date   2022-05-20
 *
 * Based on repos/gems/src/app/sculpt_manager/main.cc
 */

/*
 * Copyright (C) 2022 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <base/component.h>
#include <base/heap.h>
#include <base/attached_rom_dataspace.h>
#include <os/reporter.h>
#include <gui_session/connection.h>
#include <vm_session/vm_session.h>
#include <timer_session/connection.h>
#include <io_port_session/io_port_session.h>
#include <event_session/event_session.h>
#include <capture_session/capture_session.h>
#include <gpu_session/gpu_session.h>
#include <pin_state_session/pin_state_session.h>
#include <pin_control_session/pin_control_session.h>

/* local includes */
#include <model/runtime_state.h>
#include <model/child_exit_state.h>
#include <model/sculpt_version.h>
#include <model/file_operation_queue.h>
#include <model/index_update_queue.h>
#include <model/presets.h>
#include <menu_view.h>
#include <managed_config.h>
#include <gui.h>
#include <storage.h>
#include <network.h>
#include <deploy.h>
#include <graph.h>
#include <view/device_section_dialog.h>
#include <view/device_controls_dialog.h>
#include <view/device_power_dialog.h>
#include <view/phone_section_dialog.h>
#include <view/modem_power_dialog.h>
#include <view/pin_dialog.h>
#include <view/dialpad_dialog.h>
#include <view/current_call_dialog.h>
#include <view/outbound_dialog.h>
#include <view/storage_section_dialog.h>
#include <view/network_section_dialog.h>
#include <view/software_section_dialog.h>
#include <view/software_tabs_dialog.h>
#include <view/software_presets_dialog.h>
#include <view/software_options_dialog.h>
#include <view/software_add_dialog.h>
#include <view/software_update_dialog.h>
#include <view/software_version_dialog.h>
#include <view/software_status_dialog.h>
#include <view/download_status.h>
#include <runtime/touch_keyboard.h>

namespace Sculpt { struct Main; }


struct Sculpt::Main : Input_event_handler,
                      Dialog::Generator,
                      Runtime_config_generator,
                      Storage::Target_user,
                      Network::Action,
                      Graph::Action,
                      Dialog,
                      Depot_query,
                      Component::Construction_info,
                      Menu_view::Hover_update_handler,
                      Device_controls_dialog::Action,
                      Device_power_dialog::Action,
                      Modem_power_dialog::Action,
                      Pin_dialog::Action,
                      Dialpad_dialog::Action,
                      Current_call_dialog::Action,
                      Outbound_dialog::Action,
                      Software_presets_dialog::Action,
                      Software_options_dialog::Action,
                      Software_update_dialog::Action,
                      Software_add_dialog::Action,
                      Depot_users_dialog::Action,
                      Component::Construction_action,
                      Software_status
{
	Env &_env;

	Heap _heap { _env.ram(), _env.rm() };

	Sculpt_version const _sculpt_version { _env };

	Build_info const _build_info =
		Build_info::from_xml(Attached_rom_dataspace(_env, "build_info").xml());

	Registry<Child_state> _child_states { };

	Input::Seq_number _global_input_seq_number { };

	Gui::Connection _gui { _env, "input" };

	bool _gui_mode_ready = false;  /* becomes true once the graphics driver is up */

	Gui::Root _gui_root { _env, _heap, *this, _global_input_seq_number };

	Signal_handler<Main> _input_handler {
		_env.ep(), *this, &Main::_handle_input };

	void _handle_input()
	{
		_gui.input()->for_each_event([&] (Input::Event const &ev) {
			handle_input_event(ev); });
	}

	Managed_config<Main> _system_config {
		_env, "system", "system", *this, &Main::_handle_system_config };

	struct System
	{
		bool usb;
		bool storage;

		using State = String<32>;

		State state;

		using Power_profile = String<32>;

		Power_profile power_profile;

		unsigned brightness;

		static System from_xml(Xml_node const &node)
		{
			return System {
				.usb           = node.attribute_value("usb",     false),
				.storage       = node.attribute_value("storage", false),
				.state         = node.attribute_value("state",   State()),
				.power_profile = node.attribute_value("power_profile", Power_profile()),
				.brightness    = node.attribute_value("brightness", 0u)
			};
		}

		void generate(Xml_generator &xml) const
		{
			if (usb)     xml.attribute("usb",     "yes");
			if (storage) xml.attribute("storage", "yes");

			if (state.length() > 1)
				xml.attribute("state", state);

			if (power_profile.length() > 1)
				xml.attribute("power_profile", power_profile);

			xml.attribute("brightness", brightness);
		}

		bool operator != (System const &other) const
		{
			return (other.usb           != usb)
			    || (other.storage       != storage)
			    || (other.state         != state)
			    || (other.power_profile != power_profile)
			    || (other.brightness    != brightness);
		}

	} _system { };

	void _update_managed_system_config()
	{
		_system_config.generate([&] (Xml_generator &xml) {
			_system.generate(xml); });
	}

	void _handle_system_config(Xml_node node)
	{
		_system = System::from_xml(node);
		_update_managed_system_config();
	}

	void _enter_second_driver_stage()
	{
		/*
		 * At the first stage, we start only the drivers needed for the
		 * bare-bones GUI functionality needed to pick up a call. Once the GUI
		 * is up, we can kick off the start of the remaining drivers.
		 */

		if (_system.usb && _system.storage)
			return;

		System const orig_system = _system;

		_system.usb     = true;
		_system.storage = true;

		if (_system != orig_system)
			_update_managed_system_config();
	}

	Signal_handler<Main> _gui_mode_handler {
		_env.ep(), *this, &Main::_handle_gui_mode };

	void _handle_gui_mode();

	bool _verbose_modem = false;

	Attached_rom_dataspace _config { _env, "config" };

	Signal_handler<Main> _config_handler {
		_env.ep(), *this, &Main::_handle_config };

	void _handle_config()
	{
		_config.update();

		Xml_node const config = _config.xml();

		_verbose_modem = config.attribute_value("verbose_modem", false);
	}

	Attached_rom_dataspace _leitzentrale_rom { _env, "leitzentrale" };

	Signal_handler<Main> _leitzentrale_handler {
		_env.ep(), *this, &Main::_handle_leitzentrale };

	void _handle_leitzentrale()
	{
		_leitzentrale_rom.update();

		_leitzentrale_visible = _leitzentrale_rom.xml().attribute_value("enabled", false);

		_handle_window_layout();
	}


	/***************************
	 ** Configuration loading **
	 ***************************/

	Prepare_version _prepare_version   { 0 };
	Prepare_version _prepare_completed { 0 };

	bool _prepare_in_progress() const
	{
		return _prepare_version.value != _prepare_completed.value;
	}


	Storage _storage { _env, _heap, _child_states, *this, *this, *this };

	/**
	 * Storage::Target_user interface
	 */
	void use_storage_target(Storage_target const &target) override
	{
		_storage._sculpt_partition = target;

		/* trigger loading of the configuration from the sculpt partition */
		_prepare_version.value++;

		_deploy.restart();

		generate_runtime_config();
	}

	Pci_info _pci_info { .wifi_present  = true,
	                     .modem_present = true };

	Network _network { _env, _heap, *this, _child_states, *this, _runtime_state, _pci_info };

	/**
	 * Network::Action interface
	 */
	void update_network_dialog() override
	{
		generate_dialog();
	}


	/************
	 ** Update **
	 ************/

	Attached_rom_dataspace _update_state_rom {
		_env, "report -> runtime/update/state" };

	void _handle_update_state();

	Signal_handler<Main> _update_state_handler {
		_env.ep(), *this, &Main::_handle_update_state };

	/**
	 * Condition for spawning the update subsystem
	 */
	bool _update_running() const
	{
		return _storage._sculpt_partition.valid()
		    && !_prepare_in_progress()
		    && _network.ready()
		    && _deploy.update_needed();
	}

	Download_queue _download_queue { _heap };

	File_operation_queue _file_operation_queue { _heap };

	Fs_tool_version _fs_tool_version { 0 };

	Index_update_queue _index_update_queue {
		_heap, _file_operation_queue, _download_queue };


	/*****************
	 ** Depot query **
	 *****************/

	Depot_query::Version _query_version { 0 };

	Depot::Archive::User _image_index_user = _build_info.depot_user;

	Depot::Archive::User _index_user = _build_info.depot_user;

	Expanding_reporter _depot_query_reporter { _env, "query", "depot_query"};

	/**
	 * Depot_query interface
	 */
	Depot_query::Version depot_query_version() const override
	{
		return _query_version;
	}

	Timer::Connection _timer { _env };

	Timer::One_shot_timeout<Main> _deferred_depot_query_handler {
		_timer, *this, &Main::_handle_deferred_depot_query };

	bool _software_tab_watches_depot()
	{
		if (!_software_section_dialog.selected())
			return false;

		return _software_tabs_dialog.dialog.add_selected()
		    || _software_tabs_dialog.dialog.update_selected();
	}

	void _handle_deferred_depot_query(Duration)
	{
		if (_deploy._arch.valid()) {
			_query_version.value++;
			_depot_query_reporter.generate([&] (Xml_generator &xml) {
				xml.attribute("arch",    _deploy._arch);
				xml.attribute("version", _query_version.value);

				if (_software_tab_watches_depot() || _scan_rom.xml().has_type("empty"))
					xml.node("scan", [&] () {
						xml.attribute("users", "yes"); });

				if (_software_tab_watches_depot() || _image_index_rom.xml().has_type("empty"))
					xml.node("index", [&] () {
						xml.attribute("user",    _index_user);
						xml.attribute("version", _sculpt_version);
						xml.attribute("content", "yes");
					});

				if (_software_tab_watches_depot() || _image_index_rom.xml().has_type("empty"))
					xml.node("image_index", [&] () {
						xml.attribute("os",    "sculpt");
						xml.attribute("board", _build_info.board);
						xml.attribute("user",  _image_index_user);
					});

				_runtime_state.with_construction([&] (Component const &component) {
					xml.node("blueprint", [&] () {
						xml.attribute("pkg", component.path); }); });

				/* update query for blueprints of all unconfigured start nodes */
				_deploy.gen_depot_query(xml);
			});
		}
	}

	/**
	 * Depot_query interface
	 */
	void trigger_depot_query() override
	{
		/*
		 * Defer the submission of the query for a few milliseconds because
		 * 'trigger_depot_query' may be consecutively called several times
		 * while evaluating different conditions. Without deferring, the depot
		 * query component would produce intermediate results that take time
		 * but are ultimately discarded.
		 */
		_deferred_depot_query_handler.schedule(Microseconds{5000});
	}


	/******************
	 ** Browse index **
	 ******************/

	Attached_rom_dataspace _index_rom { _env, "report -> runtime/depot_query/index" };

	Signal_handler<Main> _index_handler {
		_env.ep(), *this, &Main::_handle_index };

	void _handle_index()
	{
		_index_rom.update();

		bool const software_add_dialog_shown = _software_section_dialog.selected()
		                                    && _software_tabs_dialog.dialog.add_selected();
		if (software_add_dialog_shown)
			generate_dialog();
	}

	/**
	 * Software_add_dialog::Action interface
	 */
	void query_index(Depot::Archive::User const &user) override
	{
		_index_user = user;
		trigger_depot_query();
	}


	/*********************
	 ** Blueprint query **
	 *********************/

	Attached_rom_dataspace _blueprint_rom { _env, "report -> runtime/depot_query/blueprint" };

	Signal_handler<Main> _blueprint_handler {
		_env.ep(), *this, &Main::_handle_blueprint };

	void _handle_blueprint()
	{
		_blueprint_rom.update();

		Xml_node const blueprint = _blueprint_rom.xml();

		/*
		 * Drop intermediate results that will be superseded by a newer query.
		 * This is important because an outdated blueprint would be disregarded
		 * by 'handle_deploy' anyway while at the same time a new query is
		 * issued. This can result a feedback loop where blueprints are
		 * requested but never applied.
		 */
		if (blueprint.attribute_value("version", 0U) != _query_version.value)
			return;

		_runtime_state.apply_to_construction([&] (Component &component) {
			component.try_apply_blueprint(blueprint); });

		_deploy.handle_deploy();
		generate_dialog();
	}


	/************
	 ** Deploy **
	 ************/

	Deploy::Prio_levels const _prio_levels { 4 };

	Attached_rom_dataspace _scan_rom { _env, "report -> runtime/depot_query/scan" };

	Signal_handler<Main> _scan_handler { _env.ep(), *this, &Main::_handle_scan };

	void _handle_scan()
	{
		_scan_rom.update();
		generate_dialog();
		_software_update_dialog.dialog.sanitize_user_selection();
	}

	Attached_rom_dataspace _image_index_rom { _env, "report -> runtime/depot_query/image_index" };

	Signal_handler<Main> _image_index_handler { _env.ep(), *this, &Main::_handle_image_index };

	void _handle_image_index()
	{
		_image_index_rom.update();
		generate_dialog();
	}

	Attached_rom_dataspace _launcher_listing_rom {
		_env, "report -> /runtime/launcher_query/listing" };

	Launchers _launchers { _heap };
	Presets   _presets   { _heap };

	Signal_handler<Main> _launcher_and_preset_listing_handler {
		_env.ep(), *this, &Main::_handle_launcher_and_preset_listing };

	void _handle_launcher_and_preset_listing()
	{
		_launcher_listing_rom.update();

		Xml_node const listing = _launcher_listing_rom.xml();
		listing.for_each_sub_node("dir", [&] (Xml_node const &dir) {

			Path const dir_path = dir.attribute_value("path", Path());

			if (dir_path == "/launcher")
				_launchers.update_from_xml(dir); /* iterate over <file> nodes */

			if (dir_path == "/presets")
				_presets.update_from_xml(dir);   /* iterate over <file> nodes */
		});

		generate_dialog();
		_deploy._handle_managed_deploy();
	}

	Deploy _deploy { _env, _heap, _child_states, _runtime_state, *this, *this, *this,
	                 _launcher_listing_rom, _blueprint_rom, _download_queue };

	Attached_rom_dataspace _manual_deploy_rom { _env, "config -> deploy" };

	void _handle_manual_deploy()
	{
		_runtime_state.reset_abandoned_and_launched_children();
		_manual_deploy_rom.update();
		_deploy.use_as_deploy_template(_manual_deploy_rom.xml());
		_deploy.update_managed_deploy_config();
	}

	Signal_handler<Main> _manual_deploy_handler {
		_env.ep(), *this, &Main::_handle_manual_deploy };


	/************
	 ** Global **
	 ************/

	Area _screen_size { };

	bool _leitzentrale_visible = false;

	Affinity::Space _affinity_space { 1, 1 };

	Sim_pin _sim_pin { };

	Modem_state _modem_state { };

	Current_call _current_call { };

	Dialed_number _dialed_number { };

	Power_state _power_state { };

	Registry<Registered<Section_dialog> > _section_dialogs { };

	/*
	 * Device section
	 */

	Device_section_dialog _device_section_dialog { _section_dialogs, _power_state };

	Conditional_float_dialog<Device_controls_dialog>
		_device_controls_dialog { "devicecontrols", _power_state, _mic_state,
		                          _audio_volume, *this };

	Conditional_float_dialog<Device_power_dialog>
		_device_power_dialog { "devicepower", _power_state, *this };

	/*
	 * Phone section
	 */

	Phone_section_dialog _phone_section_dialog { _section_dialogs, _modem_state };

	Conditional_float_dialog<Modem_power_dialog>
		_modem_power_dialog { "modempower", _modem_state, *this };

	Conditional_float_dialog<Pin_dialog>
		_pin_dialog { "pin", _sim_pin, *this };

	Conditional_float_dialog<Dialpad_dialog>
		_dialpad_dialog { "dialpad", _dialed_number, *this };

	Conditional_float_dialog<Current_call_dialog>
		_current_call_dialog { "call", _current_call, _dialed_number, *this };

	Conditional_float_dialog<Outbound_dialog>
		_outbound_dialog { "outbound", _modem_state, _current_call, *this };

	/*
	 * Storage section
	 */

	Storage_section_dialog _storage_section_dialog  { _section_dialogs };

	Storage_dialog _storage_dialog {
		_storage._storage_devices, _storage._sculpt_partition };

	/*
	 * Network section
	 */

	Network_section_dialog _network_section_dialog  {
		_section_dialogs, _network._nic_target, _network._nic_state };

	/*
	 * Software section
	 */

	Software_section_dialog _software_section_dialog { _section_dialogs, *this };

	Conditional_float_dialog<Software_tabs_dialog>
		_software_tabs_dialog { "software_tabs", _storage._sculpt_partition,
		                        _presets, *this };

	Conditional_float_dialog<Software_presets_dialog>
		_software_presets_dialog { "software_presets", _presets, *this };

	Conditional_float_dialog<Software_options_dialog>
		_software_options_dialog { "software_options", _runtime_state, _launchers, *this };

	Conditional_float_dialog<Software_add_dialog>
		_software_add_dialog { "software_add", _build_info, _sculpt_version,
		                       _network._nic_state, _index_update_queue,
		                       _index_rom, _download_queue,
		                       _cached_runtime_config,
		                       *this, _scan_rom, *this, *this, *this };

	Conditional_float_dialog<Software_update_dialog>
		_software_update_dialog { "software_update", _build_info,
		                          _network._nic_state, _download_queue,
		                          _index_update_queue, _file_operation_queue,
		                          _scan_rom, _image_index_rom, *this, *this };

	Conditional_float_dialog<Software_version_dialog>
		_software_version_dialog { "software_version", _build_info };

	Conditional_float_dialog<Software_status_dialog>
		_software_status_dialog { "software_status", *this };

	Conditional_float_dialog<Graph>
		_graph { "graph",
		         _runtime_state, _cached_runtime_config, _storage._storage_devices,
		         _storage._sculpt_partition, _storage._ram_fs_state,
		         _popup.state, _deploy._children };

	/**
	 * Dialog interface
	 */
	Hover_result hover(Xml_node) override;

	void reset() override { }

	/**
	 * Dialog interface
	 */
	void generate(Xml_generator &xml) const override
	{
		xml.node("vbox", [&] {

			_device_section_dialog.generate(xml);

			_device_controls_dialog.generate_conditional(xml, _device_section_dialog.selected());

			_device_power_dialog.generate_conditional(xml, _device_section_dialog.selected());

			if (_power_state.modem_present()) {

				_phone_section_dialog.generate(xml);
				_modem_power_dialog.generate_conditional(xml, _phone_section_dialog.selected());
				_pin_dialog.generate_conditional(xml, _phone_section_dialog.selected()
				                                   && _modem_state.ready()
				                                   && _modem_state.pin_required());

				_outbound_dialog.generate_conditional(xml, _phone_section_dialog.selected()
				                                        && _modem_state.ready()
				                                        && _modem_state.pin_ok());

				_dialpad_dialog.generate_conditional(xml, _phone_section_dialog.selected()
				                                       && _modem_state.ready()
				                                       && _modem_state.pin_ok());

				_current_call_dialog.generate_conditional(xml, _phone_section_dialog.selected()
				                                            && _modem_state.ready()
				                                            && _modem_state.pin_ok());
			}

			_storage_section_dialog.generate(xml);

			gen_named_node(xml, "float", "storage dialog", [&] {
				xml.attribute("east", "yes");
				xml.attribute("west", "yes");
				if (!_storage_section_dialog.selected())
					return;

				xml.node("frame", [&] {
					xml.node("vbox", [&] {
						_storage_dialog.gen_block_devices(xml);
						_storage_dialog.gen_usb_storage_devices(xml);
					});
				});
			});

			_network_section_dialog.generate(xml);

			gen_named_node(xml, "float", "net settings", [&] {
				xml.attribute("east", "yes");
				xml.attribute("west", "yes");
				if (_network_section_dialog.selected())
					_network.dialog.generate(xml);
			});

			_software_section_dialog.generate(xml);

			_software_tabs_dialog.generate_conditional(xml, _software_section_dialog.selected());

			_graph.generate_conditional(xml, _software_section_dialog.selected()
			                              && _software_tabs_dialog.dialog.runtime_selected());

			_software_presets_dialog.generate_conditional(xml, _software_section_dialog.selected()
			                                                && _software_tabs_dialog.dialog.presets_selected()
			                                                && _storage._sculpt_partition.valid());

			_software_options_dialog.generate_conditional(xml, _software_section_dialog.selected()
			                                                && _software_tabs_dialog.dialog.options_selected()
			                                                && _storage._sculpt_partition.valid());

			_software_add_dialog.generate_conditional(xml, _software_section_dialog.selected()
			                                            && _software_tabs_dialog.dialog.add_selected()
			                                            && _storage._sculpt_partition.valid());

			_software_update_dialog.generate_conditional(xml, _software_section_dialog.selected()
			                                               && _software_tabs_dialog.dialog.update_selected()
			                                               && _storage._sculpt_partition.valid());

			_software_version_dialog.generate_conditional(xml, _software_section_dialog.selected()
			                                                && _software_tabs_dialog.dialog.update_selected()
			                                                && !_touch_keyboard.visible);

			_software_status_dialog.generate_conditional(xml, _software_section_dialog.selected()
			                                               && _software_tabs_dialog.dialog.status_selected());

			/*
			 * Whenever the touch keyboard is visible, enforce some space at
			 * the bottom of the dialog by using a vertical stack of empty
			 * labels.
			 */
			if (_touch_keyboard.visible)
				gen_named_node(xml, "vbox", "keyboard space", [&] {
					for (unsigned i = 0; i < 15; i++)
						gen_named_node(xml, "label", String<10>(i), [&] {
							xml.attribute("text", " "); }); });
		});
	}

	/**
	 * Dialog::Generator interface
	 */
	void generate_dialog() override
	{
		/* detect need for touch keyboard */
		bool const orig_touch_keyboard_visible = _touch_keyboard.visible;

		_touch_keyboard.visible = touch_keyboard_needed();

		_main_menu_view.generate();

		if (orig_touch_keyboard_visible != _touch_keyboard.visible)
			_handle_window_layout();
	}

	Attached_rom_dataspace _runtime_state_rom { _env, "report -> runtime/state" };

	Runtime_state _runtime_state { _heap, _storage._sculpt_partition };

	Managed_config<Main> _runtime_config {
		_env, "config", "runtime", *this, &Main::_handle_runtime };

	/**
	 * Component::Construction_info interface
	 */
	void _with_construction(Component::Construction_info::With const &fn) const override
	{
		_runtime_state.with_construction([&] (Component const &c) { fn.with(c); });
	}

	/**
	 * Component::Construction_action interface
	 */
	void new_construction(Component::Path const &pkg, Verify verify,
	                      Component::Info const &info) override
	{
		(void)_runtime_state.new_construction(pkg, verify, info, _affinity_space);
		trigger_depot_query();
	}

	void _apply_to_construction(Component::Construction_action::Apply_to &fn) override
	{
		_runtime_state.apply_to_construction([&] (Component &c) { fn.apply_to(c); });
	}

	/**
	 * Component::Construction_action interface
	 */
	void trigger_pkg_download() override
	{
		_runtime_state.apply_to_construction([&] (Component &c) {
			_download_queue.add(c.path, c.verify); });

		/* incorporate new download-queue content into update */
		_deploy.update_installation();

		generate_runtime_config();
	}

	/**
	 * Component::Construction_action interface
	 */
	void discard_construction() override { _runtime_state.discard_construction(); }

	/**
	 * Component::Construction_action interface
	 */
	void launch_construction()  override
	{
		_runtime_state.launch_construction();

		/* trigger change of the deployment */
		_deploy.update_managed_deploy_config();
	}

	bool _manually_managed_runtime = false;

	void _handle_runtime(Xml_node config)
	{
		_manually_managed_runtime = !config.has_type("empty");
		generate_runtime_config();
		generate_dialog();
	}

	void _generate_runtime_config(Xml_generator &) const;

	/**
	 * Runtime_config_generator interface
	 */
	void generate_runtime_config() override
	{
		if (!_runtime_config.try_generate_manually_managed())
			_runtime_config.generate([&] (Xml_generator &xml) {
				_generate_runtime_config(xml); });
	}

	Signal_handler<Main> _runtime_state_handler {
		_env.ep(), *this, &Main::_handle_runtime_state };

	void _handle_runtime_state();

	Attached_rom_dataspace const _platform { _env, "platform_info" };


	/********************
	 ** Touch keyboard **
	 ********************/

	struct Touch_keyboard : Noncopyable
	{
		/*
		 * Spawn the leitzentrale touch keyboard only after the basic GUI is up
		 * beacuse the touch keyboard is not needed to pick up a call.
		 */
		bool started = false;

		/*
		 * Updated and evaluated by 'generate_dialog'
		 */
		bool visible = false;

		Touch_keyboard_attr attr;

		Touch_keyboard(Touch_keyboard_attr attr) : attr(attr) { };

		void gen_start_node(Xml_generator &xml) const
		{
			if (started)
				gen_touch_keyboard(xml, attr);
		}
	};

	Touch_keyboard _touch_keyboard {
		.attr = { .min_width  = 720,
		          .min_height = 480,
		          .alpha      = Menu_view::Alpha::OPAQUE,
		          .background = _background_color } };

	bool _depot_user_selection_visible() const
	{
		if (!_software_section_dialog.selected())
			return false;

		return _software_tabs_dialog.dialog.update_selected()
		    || _software_tabs_dialog.dialog.add_selected();
	}

	bool _software_add_dialog_has_keyboard_focus() const
	{
		return _software_section_dialog.selected()
		    && _software_tabs_dialog.dialog.add_selected()
		    && _software_add_dialog.dialog.keyboard_needed();
	}

	bool _software_update_dialog_has_keyboard_focus() const
	{
		return _software_section_dialog.selected()
		    && _software_tabs_dialog.dialog.update_selected()
		    && _software_update_dialog.dialog.keyboard_needed();
	}

	bool _network_dialog_has_keyboard_focus() const
	{
		return _network_section_dialog.selected()
		    && _network.dialog.need_keyboard_focus_for_passphrase();
	}

	/**
	 * Condition for controlling the visibility of the touch keyboard
	 */
	bool touch_keyboard_needed() const
	{
		return _software_add_dialog_has_keyboard_focus()
		    || _software_update_dialog_has_keyboard_focus()
		    || _network_dialog_has_keyboard_focus();
	}


	/****************************************
	 ** Cached model of the runtime config **
	 ****************************************/

	/*
	 * Even though the runtime configuration is generated by the sculpt
	 * manager, we still obtain it as a separate ROM session to keep the GUI
	 * part decoupled from the lower-level runtime configuration generator.
	 */
	Attached_rom_dataspace _runtime_config_rom { _env, "config -> managed/runtime" };

	Signal_handler<Main> _runtime_config_handler {
		_env.ep(), *this, &Main::_handle_runtime_config };

	Runtime_config _cached_runtime_config { _heap };

	void _handle_runtime_config()
	{
		_runtime_config_rom.update();
		_cached_runtime_config.update_from_xml(_runtime_config_rom.xml());
		generate_dialog(); /* update graph */
	}


	/****************************
	 ** Interactive operations **
	 ****************************/

	Constructible<Input::Seq_number> _clicked_seq_number { };
	Constructible<Input::Seq_number> _clacked_seq_number { };

	void _section_enabled(Section_dialog &section, bool enabled)
	{
		/* reset all sections to the default */
		_section_dialogs.for_each([&] (Section_dialog &dialog) {
			dialog.detail = Section_dialog::Detail::DEFAULT; });

		/* select specified section if enabled */
		bool any_selected = false;
		_section_dialogs.for_each([&] (Section_dialog &dialog) {
			if ((&dialog == &section) && enabled) {
				dialog.detail = Section_dialog::Detail::SELECTED;
				any_selected = true;
			}
		});

		/* minimize unselected sections if any section is selected */
		_section_dialogs.for_each([&] (Section_dialog &dialog) {
			if (dialog.detail != Section_dialog::Detail::SELECTED)
				dialog.detail = any_selected
				              ? Section_dialog::Detail::MINIMIZED
				              : Section_dialog::Detail::DEFAULT; });
	}

	void _try_handle_click()
	{
		if (!_clicked_seq_number.constructed())
			return;

		Input::Seq_number const seq = *_clicked_seq_number;

		if (_main_menu_view.hovered(seq)) {

			/* determine clicked section */
			Section_dialog *clicked_ptr = nullptr;
			_section_dialogs.for_each([&] (Section_dialog &dialog) {
				if (dialog.hovered())
					clicked_ptr = &dialog; });

			/* toggle clicked section dialog */
			if (clicked_ptr)
				_section_enabled(*clicked_ptr, !clicked_ptr->selected());

			if (_device_controls_dialog.hovered())
				_device_controls_dialog.click();

			if (_device_power_dialog.hovered())
				_device_power_dialog.click();

			if (_modem_power_dialog.hovered())
				_modem_power_dialog.click();

			if (_pin_dialog.hovered())
				_pin_dialog.click();

			if (_dialpad_dialog.hovered())
				_dialpad_dialog.click();

			if (_outbound_dialog.hovered())
				_outbound_dialog.click();

			if (_current_call_dialog.hovered())
				_current_call_dialog.click();

			if (_storage_dialog.hovered)
				_storage_dialog.click(*this);

			if (_network.dialog.hovered)
				_network.dialog.click(_network);

			if (_software_tabs_dialog.hovered()) {
				_software_tabs_dialog.click();

				/* refresh list of depot users */
				trigger_depot_query();
			}

			if (_graph.hovered())
				_graph.dialog.click(*this);

			if (_software_presets_dialog.hovered())
				_software_presets_dialog.click();

			if (_software_options_dialog.hovered())
				_software_options_dialog.click();

			if (_software_add_dialog.hovered())
				_software_add_dialog.click();

			if (_software_update_dialog.hovered())
				_software_update_dialog.click();

			_clicked_seq_number.destruct();
			generate_dialog();
		}
	}

	void _try_handle_clack()
	{
		if (!_clacked_seq_number.constructed())
			return;

		Input::Seq_number const seq = *_clacked_seq_number;

		if (_main_menu_view.hovered(seq)) {

			_device_controls_dialog.clack();
			_device_power_dialog.clack();
			_pin_dialog.clack();
			_dialpad_dialog.clack();
			_current_call_dialog.clack();
			_software_presets_dialog.clack();
			_software_add_dialog.clack();
			_software_update_dialog.clack();

			if (_storage_dialog.hovered)
				_storage_dialog.clack(*this);

			if (_graph.hovered())
				_graph.dialog.clack(*this, _storage);

			_clacked_seq_number.destruct();
			generate_dialog();
		}
	}

	/**
	 * Menu_view::Hover_update_handler interface
	 */
	void menu_view_hover_updated() override
	{
		/* take first hover report as indicator that the GUI is ready */
		if (!_system.storage && _runtime_state.present_in_runtime("menu_view")) {
			_enter_second_driver_stage();

			/* once the basic GUI is up, it is time to spawn the touch keyboard */
			_touch_keyboard.started = true;
			generate_runtime_config();
		}

		if (_clicked_seq_number.constructed())
			_try_handle_click();

		if (_clacked_seq_number.constructed())
			_try_handle_clack();
	}

	/* true while touched, used to issue only one click per touch sequence */
	bool _touched = false;

	/**
	 * Input_event_handler interface
	 */
	void handle_input_event(Input::Event const &ev) override
	{
		bool need_generate_dialog = false;

		if (ev.key_press(Input::BTN_LEFT) || ev.touch()) {
			if (!_touched) {
				_clicked_seq_number.construct(_global_input_seq_number);
				_try_handle_click();
				_touched = true;
			}
		}

		if (ev.key_release(Input::BTN_LEFT) || ev.touch_release()) {
			_clacked_seq_number.construct(_global_input_seq_number);
			_try_handle_clack();
			_touched = false;
		}

		ev.handle_press([&] (Input::Keycode, Codepoint code) {

			need_generate_dialog = true;
			if (_software_add_dialog_has_keyboard_focus())
				_software_add_dialog.dialog.handle_key(code);
			else if (_software_update_dialog_has_keyboard_focus())
				_software_update_dialog.dialog.handle_key(code);
			else if (_network_dialog_has_keyboard_focus())
				_network.handle_key_press(code);
		});

		if (need_generate_dialog)
			generate_dialog();
	}

	Color const _background_color { 62, 62, 67, 255 };

	Menu_view _main_menu_view { _env, _child_states, *this, "menu_view",
	                             Ram_quota{12*1024*1024}, Cap_quota{150},
	                             "menu_dialog", "menu_view_hover", *this,
	                             Menu_view::Alpha::OPAQUE,
	                             _background_color };

	void _handle_window_layout();

	template <size_t N, typename FN>
	void _with_window(Xml_node window_list, String<N> const &match, FN const &fn)
	{
		window_list.for_each_sub_node("window", [&] (Xml_node win) {
			if (win.attribute_value("label", String<N>()) == match)
				fn(win); });
	}

	Attached_rom_dataspace _window_list { _env, "window_list" };

	Signal_handler<Main> _window_list_handler {
		_env.ep(), *this, &Main::_handle_window_layout };

	Expanding_reporter _wm_focus { _env, "focus", "wm_focus" };

	Attached_rom_dataspace _decorator_margins { _env, "decorator_margins" };

	Signal_handler<Main> _decorator_margins_handler {
		_env.ep(), *this, &Main::_handle_window_layout };

	Expanding_reporter _window_layout { _env, "window_layout", "window_layout" };

	void _reset_storage_dialog_operation()
	{
		_graph.dialog.reset_storage_operation();
		_storage_dialog.reset_operation();
	}

	/*
	 * Fs_dialog::Action interface
	 */
	void toggle_inspect_view(Storage_target const &) override { }

	void use(Storage_target const &target) override
	{
		_software_update_dialog.dialog.reset();
		_download_queue.reset();
		_storage.use(target);
	}

	/*
	 * Storage_dialog::Action interface
	 */
	void format(Storage_target const &target) override
	{
		_storage.format(target);
	}

	void cancel_format(Storage_target const &target) override
	{
		_storage.cancel_format(target);
		_reset_storage_dialog_operation();
	}

	void expand(Storage_target const &target) override
	{
		_storage.expand(target);
	}

	void cancel_expand(Storage_target const &target) override
	{
		_storage.cancel_expand(target);
		_reset_storage_dialog_operation();
	}

	void check(Storage_target const &target) override
	{
		_storage.check(target);
	}

	void toggle_default_storage_target(Storage_target const &target) override
	{
		_storage.toggle_default_storage_target(target);
	}

	/*
	 * Graph::Action interface
	 */
	void remove_deployed_component(Start_name const &name) override
	{
		_runtime_state.abandon(name);

		/* update config/managed/deploy with the component 'name' removed */
		_deploy.update_managed_deploy_config();
	}

	/*
	 * Graph::Action interface
	 */
	void restart_deployed_component(Start_name const &name) override
	{
		if (name == "nic_drv") {

			_network.restart_nic_drv_on_next_runtime_cfg();
			generate_runtime_config();

		} else if (name == "wifi_drv") {

			_network.restart_wifi_drv_on_next_runtime_cfg();
			generate_runtime_config();

		} else if (name == "usb_net") {

			_network.restart_usb_net_on_next_runtime_cfg();
			generate_runtime_config();

		} else {

			_runtime_state.restart(name);

			/* update config/managed/deploy with the component 'name' removed */
			_deploy.update_managed_deploy_config();
		}
	}

	/*
	 * Graph::Action interface
	 */
	void toggle_launcher_selector(Rect) override { }

	bool _network_missing() const {
		return _deploy.update_needed() && !_network._nic_state.ready(); }

	bool _diagnostics_available() const {
		return _deploy.any_unsatisfied_child() || _network_missing(); }

	/**
	 * Software_status interface
	 */
	bool software_status_available() const override
	{
		return _diagnostics_available()
		    || _update_running()
		    || _download_queue.any_failed_download();
	}

	/**
	 * Software_status interface
	 */
	Software_status::Message software_status_message() const override
	{
		if (_update_running())
			return "install ...";

		if (_diagnostics_available())
			return "!";

		return " ";
	}

	/**
	 * Software_status interface
	 */
	void generate_software_status(Xml_generator &xml) const override
	{
		xml.node("vbox", [&] () {
			if (_manually_managed_runtime)
				return;

			auto gen_network_diagnostics = [&] (Xml_generator &xml)
			{
				if (!_network_missing())
					return;

				gen_named_node(xml, "hbox", "network", [&] () {
					gen_named_node(xml, "float", "left", [&] () {
						xml.attribute("west", "yes");
						xml.node("label", [&] () {
							xml.attribute("text", "network needed for installation");
							xml.attribute("font", "annotation/regular");
						});
					});
				});
			};

			if (_diagnostics_available()) {
				gen_named_node(xml, "frame", "diagnostics", [&] () {
					xml.node("vbox", [&] () {

						gen_named_node(xml, "label", "top", [&] () {
							xml.attribute("min_ex", "40");
							xml.attribute("text", ""); });

						xml.node("label", [&] () {
							xml.attribute("text", "Diagnostics"); });

						xml.node("float", [&] () {
							xml.node("vbox", [&] () {
								gen_network_diagnostics(xml);
								_deploy.gen_child_diagnostics(xml);
							});
						});

						gen_named_node(xml, "label", "bottom", [&] () {
							xml.attribute("text", " "); });
					});
				});
			}

			Xml_node const state = _update_state_rom.xml();

			bool const download_in_progress =
				(_update_running() && state.attribute_value("progress", false));

			if (download_in_progress || _download_queue.any_failed_download())
				gen_download_status(xml, state, _download_queue);
		});
	}


	/**
	 * Software_presets_dialog::Action interface
	 */
	void load_deploy_preset(Presets::Info::Name const &name) override
	{
		Xml_node const listing = _launcher_listing_rom.xml();

		listing.for_each_sub_node("dir", [&] (Xml_node const &dir) {
			if (dir.attribute_value("path", Path()) == "/presets") {
				dir.for_each_sub_node("file", [&] (Xml_node const &file) {
					if (file.attribute_value("name", Presets::Info::Name()) == name) {
						file.with_optional_sub_node("config", [&] (Xml_node const &config) {
							_runtime_state.reset_abandoned_and_launched_children();
							_deploy.use_as_deploy_template(config);
							_deploy.update_managed_deploy_config();
						});
					}
				});
			}
		});
	}

	/**
	 * Software_options_dialog::Action interface
	 */
	void enable_optional_component(Path const &launcher) override
	{
		_runtime_state.launch(launcher, launcher);

		/* trigger change of the deployment */
		_deploy.update_managed_deploy_config();
	}

	/**
	 * Software_options_dialog::Action interface
	 */
	void disable_optional_component(Path const &launcher) override
	{
		_runtime_state.abandon(launcher);

		/* update config/managed/deploy with the component 'name' removed */
		_deploy.update_managed_deploy_config();
	}

	/**
	 * Depot_users_dialog::Action interface
	 */
	void add_depot_url(Depot_url const &depot_url) override
	{
		using Content = File_operation_queue::Content;

		_file_operation_queue.new_small_file(Path("/rw/depot/", depot_url.user, "/download"),
		                                     Content { depot_url.download });

		if (!_file_operation_queue.any_operation_in_progress())
			_file_operation_queue.schedule_next_operations();

		generate_runtime_config();
	}

	/**
	 * Software_update_dialog::Action interface
	 */
	void query_image_index(Depot::Archive::User const &user) override
	{
		_image_index_user = user;
		trigger_depot_query();
	}

	/**
	 * Software_update_dialog::Action interface
	 */
	void trigger_image_download(Path const &path, Verify verify) override
	{
		_download_queue.remove_inactive_downloads();
		_download_queue.add(path, verify);
		_deploy.update_installation();
		generate_runtime_config();
	}

	/**
	 * Software_update_dialog::Action interface
	 */
	void update_image_index(Depot::Archive::User const &user, Verify verify) override
	{
		_download_queue.remove_inactive_downloads();
		_index_update_queue.remove_inactive_updates();
		_index_update_queue.add(Path(user, "/image/index"), verify);
		generate_runtime_config();
	}

	/**
	 * Software_update_dialog::Action interface
	 */
	void install_boot_image(Path const &path) override
	{
		_file_operation_queue.copy_all_files(Path("/rw/depot/", path), "/rw/boot");

		if (!_file_operation_queue.any_operation_in_progress())
			_file_operation_queue.schedule_next_operations();

		generate_runtime_config();
	}

	/**
	 * Software_add_dialog::Action interface
	 */
	void update_sculpt_index(Depot::Archive::User const &user, Verify verify) override
	{
		_download_queue.remove_inactive_downloads();
		_index_update_queue.remove_inactive_updates();
		_index_update_queue.add(Path(user, "/index/", _sculpt_version), verify);
		generate_runtime_config();
	}


	/***********
	 ** Audio **
	 ***********/

	Mic_state _mic_state = Mic_state::PHONE;

	Audio_volume _audio_volume { .value = 75 };

	Expanding_reporter _audio_config { _env, "config", "audio_config" };

	struct Audio_config
	{
		bool earpiece, speaker, mic, modem;

		Audio_volume audio_volume;

		bool operator != (Audio_config const &other) const
		{
			return (earpiece           != other.earpiece)
			    || (speaker            != other.speaker)
			    || (mic                != other.mic)
			    || (modem              != other.modem)
			    || (audio_volume.value != other.audio_volume.value);
		}

		void generate(Xml_generator &xml) const
		{
			xml.node("earpiece", [&] () {
				xml.attribute("volume", earpiece ? 100 : 0);
			});
			xml.node("speaker", [&] () {
				xml.attribute("volume", speaker  ? audio_volume.value : 0);
			});
			xml.node("mic", [&] () {
				xml.attribute("volume", mic ? 80 : 0);
			});
			xml.node("codec", [&]() {
				xml.attribute("target", modem ? "modem" : "soc");
			});
		}
	};

	Audio_config _curr_audio_config { };

	void _generate_audio_config()
	{
		auto mic_enabled = [&]
		{
			switch (_mic_state) {
			case Mic_state::OFF:   return false;
			case Mic_state::PHONE: return _current_call.active();
			case Mic_state::ON:    return true;
			}
			return false;
		};

		Audio_config const new_config {

			.earpiece = true,

			/* enable speaker for the ring tone when no call is active */
			.speaker  = !_current_call.active() || _current_call.speaker,

			/* enable microphone during call */
			.mic = mic_enabled(),

			/* set codec target during call */
			.modem = _current_call.active(),

			.audio_volume = _audio_volume
		};

		if (new_config != _curr_audio_config) {
			_curr_audio_config = new_config;
			_audio_config.generate([&] (Xml_generator &xml) {
				_curr_audio_config.generate(xml); });
		}
	}

	/**
	 * Device_controls_dialog::Action interface
	 */
	void select_volume_level(unsigned level) override
	{
		_audio_volume.value = level;
		_generate_audio_config();
	}

	/**
	 * Device_controls_dialog::Action interface
	 */
	void select_mic_policy(Mic_state const &policy) override
	{
		_mic_state = policy;
		_generate_audio_config();
	}


	/**********************
	 ** Device functions **
	 **********************/

	Attached_rom_dataspace _power_rom { _env, "report -> drivers/power" };

	Signal_handler<Main> _power_handler {
		_env.ep(), *this, &Main::_handle_power };

	void _handle_power()
	{
		_power_rom.update();

		Power_state const orig_power_state = _power_state;
		_power_state = Power_state::from_xml(_power_rom.xml());

		bool regenerate_dialog = false;

		/* mobile data connectivity depends on the presence of a battery */
		if (_power_state.modem_present() != _pci_info.modem_present) {

			/* update condition for the "Mobile data" network option */
			_pci_info.modem_present = _power_state.modem_present()
			                       && _modem_state.ready();

			regenerate_dialog = true;
		}

		if (orig_power_state.summary() != _power_state.summary())
			regenerate_dialog = true;

		if (_device_section_dialog.selected())
			regenerate_dialog = true;

		if (regenerate_dialog)
			generate_dialog();
	}

	/**
	 * Device_controls_dialog::Action interface
	 */
	void select_brightness_level(unsigned level) override
	{
		_system.brightness = level;
		_update_managed_system_config();
	}

	/**
	 * Device_power_dialog::Action interface
	 */
	void activate_performance_power_profile() override
	{
		_system.power_profile = "performance";
		_update_managed_system_config();
	}

	/**
	 * Device_power_dialog::Action interface
	 */
	void activate_economic_power_profile() override
	{
		_system.power_profile = "economic";
		_update_managed_system_config();
	}

	/**
	 * Device_power_dialog::Action interface
	 */
	void trigger_device_reboot() override
	{
		_system.state = "reset";
		_update_managed_system_config();
	}

	/**
	 * Device_power_dialog::Action interface
	 */
	void trigger_device_off() override
	{
		_system.state = "poweroff";
		_update_managed_system_config();
	}


	/***********
	 ** Phone **
	 ***********/

	Expanding_reporter _modem_config { _env, "config", "modem_config" };

	enum class Modem_config_power { ANY, OFF, ON };

	Modem_config_power _modem_config_power { Modem_config_power::ANY };

	/*
	 * State that influences the modem configuration, used to detect the
	 * need for configuraton updates.
	 */
	struct Modem_config
	{
		Modem_config_power modem_power;
		Modem_state        modem_state;
		Sim_pin            sim_pin;
		Current_call       current_call;

		bool operator != (Modem_config const &other) const
		{
			return (modem_power  != other.modem_power)
			    || (modem_state  != other.modem_state)
			    || (sim_pin      != other.sim_pin)
			    || (current_call != other.current_call);
		}

		void generate(Xml_generator &xml) const
		{
			switch (modem_power) {
			case Modem_config_power::OFF: xml.attribute("power", "off"); break;
			case Modem_config_power::ON:  xml.attribute("power", "on");  break;
			case Modem_config_power::ANY: break;
			}

			bool const supply_pin = modem_state.pin_required()
			                     && sim_pin.suitable_for_unlock()
			                     && sim_pin.confirmed;
			if (supply_pin)
				xml.attribute("pin", String<10>(sim_pin));

			xml.node("ring", [&] {
				xml.append_content("AT+QLDTMF=5,\"4,3,6,#,D,3\",1"); });

			current_call.gen_modem_config(xml);
		}
	};

	Modem_config _curr_modem_config { };

	Attached_rom_dataspace _modem_state_rom { _env, "report -> drivers/modem/state" };

	Signal_handler<Main> _modem_state_handler {
		_env.ep(), *this, &Main::_handle_modem_state };

	void _handle_modem_state()
	{
		_modem_state_rom.update();

		if (_verbose_modem)
			log("modem state: ", _modem_state_rom.xml());

		Modem_state const orig_modem_state = _modem_state;

		bool regenerate_dialog = false;

		_modem_state = Modem_state::from_xml(_modem_state_rom.xml());

		/* update condition of "Mobile data" network option */
		{
			bool const orig_mobile_data_ready = _pci_info.modem_present;
			_pci_info.modem_present = _power_state.modem_present()
			                       && _modem_state.ready();
			if (orig_mobile_data_ready != _pci_info.modem_present)
				regenerate_dialog = true;
		}

		_current_call.update(_modem_state);

		if (_modem_state.pin_rejected())
			_sim_pin = Sim_pin { };

		bool const configured_current_call_out_of_date =
			(_current_call != _curr_modem_config.current_call);

		bool const modem_state_changed = (orig_modem_state != _modem_state);

		if (configured_current_call_out_of_date || modem_state_changed) {
			_generate_modem_config();
			regenerate_dialog = true;
		}

		if (regenerate_dialog)
			generate_dialog();
	}

	void _generate_modem_config()
	{
		Modem_config const new_config {
			.modem_power  = _modem_config_power,
			.modem_state  = _modem_state,
			.sim_pin      = _sim_pin,
			.current_call = _current_call,
		};

		if (new_config != _curr_modem_config) {

			_curr_modem_config = new_config;

			_modem_config.generate([&] (Xml_generator &xml) {

				if (_verbose_modem)
					xml.attribute("verbose", "yes");

				_curr_modem_config.generate(xml);
			});
		}

		/* update audio config as it depends on the current call state */
		_generate_audio_config();
	}

	/**
	 * Modem_power_dialog::Action interface
	 */
	void modem_power(bool enabled) override
	{
		_modem_config_power = enabled ? Modem_config_power::ON
		                              : Modem_config_power::OFF;

		/* forget pin and call state when powering off the modem */
		if (!enabled) {
			_sim_pin      = { };
			_current_call = { };
		}

		_generate_modem_config();
	}

	/**
	 * Pin_dialog::Action interface
	 */
	void append_sim_pin_digit(Sim_pin::Digit d) override
	{
		_sim_pin.append_digit(d);
	}

	/**
	 * Pin_dialog::Action interface
	 */
	void remove_last_sim_pin_digit() override
	{
		_sim_pin.remove_last_digit();
	}

	/**
	 * Pin_dialog::Action interface
	 */
	void confirm_sim_pin() override
	{
		if (_sim_pin.suitable_for_unlock())
			_sim_pin.confirmed = true;
		_generate_modem_config();
	}

	/**
	 * Dialpad_dialog::Action interface
	 */
	void append_dial_digit(Dialed_number::Digit d) override
	{
		_dialed_number.append_digit(d);
	}

	/**
	 * Dialpad_dialog::Action interface
	 */
	void remove_last_dial_digit() override
	{
		_dialed_number.remove_last_digit();
	}

	/**
	 * Current_call_dialog::Action interface
	 */
	void accept_incoming_call() override
	{
		_current_call.accept();
		_generate_modem_config();
	}

	/**
	 * Current_call_dialog::Action interface
	 */
	void reject_incoming_call() override
	{
		_current_call.reject();
		_generate_modem_config();
	}

	/**
	 * Current_call_dialog::Action interface
	 */
	void hang_up() override
	{
		_current_call.reject();
		_generate_modem_config();
	}

	/**
	 * Current_call_dialog::Action interface
	 */
	void toggle_speaker() override
	{
		_current_call.toggle_speaker();
		_generate_modem_config();
	}

	/**
	 * Current_call_dialog::Action interface
	 */
	void initiate_call() override
	{
		if (_dialed_number.suitable_for_call()) {
			_current_call.initiate(Number(_dialed_number));
			_generate_modem_config();
		}
	}

	/**
	 * Current_call_dialog::Action interface
	 */
	void cancel_initiated_call() override
	{
		_current_call.cancel();
		_generate_modem_config();
	}


	/*******************
	 ** Runtime graph **
	 *******************/

	Popup _popup { };

	Main(Env &env) : _env(env)
	{
		_config.sigh(_config_handler);
		_leitzentrale_rom.sigh(_leitzentrale_handler);
		_manual_deploy_rom.sigh(_manual_deploy_handler);
		_runtime_state_rom.sigh(_runtime_state_handler);
		_runtime_config_rom.sigh(_runtime_config_handler);
		_gui.input()->sigh(_input_handler);
		_gui.mode_sigh(_gui_mode_handler);

		/*
		 * Subscribe to reports
		 */
		_update_state_rom    .sigh(_update_state_handler);
		_window_list         .sigh(_window_list_handler);
		_decorator_margins   .sigh(_decorator_margins_handler);
		_scan_rom            .sigh(_scan_handler);
		_launcher_listing_rom.sigh(_launcher_and_preset_listing_handler);
		_blueprint_rom       .sigh(_blueprint_handler);
		_image_index_rom     .sigh(_image_index_handler);
		_power_rom           .sigh(_power_handler);
		_modem_state_rom     .sigh(_modem_state_handler);
		_index_rom           .sigh(_index_handler);

		/*
		 * Import initial report content
		 */
		_handle_config();
		_handle_leitzentrale();
		_handle_gui_mode();
		_storage.handle_storage_devices_update();
		_handle_runtime_config();
		_handle_modem_state();

		_system_config.with_manual_config([&] (Xml_node const &system) {
			_system = System::from_xml(system); });

		/*
		 * Read static platform information
		 */
		_platform.xml().with_optional_sub_node("affinity-space", [&] (Xml_node const &node) {
			_affinity_space = Affinity::Space(node.attribute_value("width",  1U),
			                                  node.attribute_value("height", 1U));
		});

		/*
		 * Generate initial config/managed/deploy configuration
		 */
		_handle_manual_deploy();

		_generate_modem_config();
		generate_runtime_config();
		generate_dialog();
	}
};


void Sculpt::Main::_handle_window_layout()
{
	/* skip window-layout handling (and decorator activity) while booting */
	if (!_gui_mode_ready)
		return;

	struct Decorator_margins
	{
		unsigned top = 0, bottom = 0, left = 0, right = 0;

		Decorator_margins(Xml_node node)
		{
			if (!node.has_sub_node("floating"))
				return;

			Xml_node const floating = node.sub_node("floating");

			top    = floating.attribute_value("top",    0U);
			bottom = floating.attribute_value("bottom", 0U);
			left   = floating.attribute_value("left",   0U);
			right  = floating.attribute_value("right",  0U);
		}
	};

	/* read decorator margins from the decorator's report */
	_decorator_margins.update();
	Decorator_margins const margins(_decorator_margins.xml());

	typedef String<128> Label;
	Label const menu_view_label     ("runtime -> leitzentrale -> menu_view");
	Label const touch_keyboard_label("runtime -> leitzentrale -> touch_keyboard");

	_window_list.update();
	Xml_node const window_list = _window_list.xml();

	auto win_size = [&] (Xml_node win) {
		return Area(win.attribute_value("width",  0U),
		            win.attribute_value("height", 0U)); };

	Framebuffer::Mode const mode = _gui.mode();

	/* suppress intermediate boot-time states before the framebuffer driver is up */
	if (mode.area.count() <= 1)
		return;

	_window_layout.generate([&] (Xml_generator &xml) {

		auto gen_window = [&] (Xml_node win, Rect rect) {
			if (rect.valid()) {
				xml.node("window", [&] () {
					xml.attribute("id",     win.attribute_value("id", 0UL));
					xml.attribute("xpos",   rect.x1());
					xml.attribute("ypos",   rect.y1());
					xml.attribute("width",  rect.w());
					xml.attribute("height", rect.h());
					xml.attribute("title",  win.attribute_value("label", Label()));
				});
			}
		};

		_with_window(window_list, touch_keyboard_label, [&] (Xml_node win) {
			if (!_leitzentrale_visible)
				return;

			Area  const size = win_size(win);
			Point const pos  = _touch_keyboard.visible
			                 ? Point(0, int(mode.area.h()) - int(size.h()))
			                 : Point(0, int(mode.area.h()));

			gen_window(win, Rect(pos, size));
		});

		_with_window(window_list, menu_view_label, [&] (Xml_node win) {
			Area  const size = win_size(win);
			Point const pos(_leitzentrale_visible ? 0 : int(size.w()), 0);
			gen_window(win, Rect(pos, size));
		});
	});
}


void Sculpt::Main::_handle_gui_mode()
{
	Framebuffer::Mode const mode = _gui.mode();

	if (mode.area.count() > 1)
		_gui_mode_ready = true;

	_handle_window_layout();

	_screen_size = mode.area;
	_main_menu_view.min_width  = _screen_size.w();
	_main_menu_view.min_height = _screen_size.h();

	generate_runtime_config();
}


Sculpt::Dialog::Hover_result Sculpt::Main::hover(Xml_node hover)
{
	Hover_result result = Hover_result::UNMODIFIED;

	_section_dialogs.for_each([&] (Section_dialog &dialog) {
		result = any_hover_changed(result, dialog.hover(Xml_node("<empty/>"))); });

	hover.with_optional_sub_node("vbox", [&] (Xml_node const &vbox) {
		_section_dialogs.for_each([&] (Section_dialog &dialog) {
			result = any_hover_changed(result, dialog.hover(vbox)); });

		result = any_hover_changed(result,
			_device_controls_dialog .hover(vbox),
			_device_power_dialog    .hover(vbox),
			_modem_power_dialog     .hover(vbox),
			_pin_dialog             .hover(vbox),
			_dialpad_dialog         .hover(vbox),
			_current_call_dialog    .hover(vbox),
			_outbound_dialog        .hover(vbox),
			_graph                  .hover(vbox),
			_software_tabs_dialog   .hover(vbox),
			_software_presets_dialog.hover(vbox),
			_software_options_dialog.hover(vbox),
			_software_add_dialog    .hover(vbox),
			_software_update_dialog .hover(vbox),
			_storage_dialog.match_sub_dialog(vbox, "float", "frame", "vbox"),
			_network.dialog.match_sub_dialog(vbox, "float")
		);
	});

	return result;
}


void Sculpt::Main::_handle_update_state()
{
	_update_state_rom.update();

	Xml_node const update_state = _update_state_rom.xml();

	_download_queue.apply_update_state(update_state);
	bool const any_completed_download = _download_queue.any_completed_download();
	_download_queue.remove_completed_downloads();

	_index_update_queue.apply_update_state(update_state);

	bool const installation_complete =
		!update_state.attribute_value("progress", false);

	if (installation_complete) {

		Xml_node const blueprint = _blueprint_rom.xml();
		bool const new_depot_query_needed = blueprint_any_missing(blueprint)
		                                 || blueprint_any_rom_missing(blueprint)
		                                 || any_completed_download;
		if (new_depot_query_needed)
			trigger_depot_query();

		_deploy.reattempt_after_installation();
	}

	generate_dialog();
}


void Sculpt::Main::_handle_runtime_state()
{
	_runtime_state_rom.update();

	Xml_node state = _runtime_state_rom.xml();

	_runtime_state.update_from_state_report(state);

	bool reconfigure_runtime = false;
	bool regenerate_dialog   = false;

	/* check for completed storage operations */
	_storage._storage_devices.for_each([&] (Storage_device &device) {

		device.for_each_partition([&] (Partition &partition) {

			Storage_target const target { device.label, partition.number };

			if (partition.check_in_progress) {
				String<64> name(target.label(), ".e2fsck");
				Child_exit_state exit_state(state, name);

				if (exit_state.exited) {
					if (exit_state.code != 0)
						error("file-system check failed");
					if (exit_state.code == 0)
						log("file-system check succeeded");

					partition.check_in_progress = 0;
					reconfigure_runtime = true;
					_reset_storage_dialog_operation();
				}
			}

			if (partition.format_in_progress) {
				String<64> name(target.label(), ".mke2fs");
				Child_exit_state exit_state(state, name);

				if (exit_state.exited) {
					if (exit_state.code != 0)
						error("file-system creation failed");

					partition.format_in_progress = false;
					partition.file_system.type = File_system::EXT2;

					if (partition.whole_device())
						device.rediscover();

					reconfigure_runtime = true;
					_reset_storage_dialog_operation();
				}
			}

			/* respond to completion of file-system resize operation */
			if (partition.fs_resize_in_progress) {
				Child_exit_state exit_state(state, Start_name(target.label(), ".resize2fs"));
				if (exit_state.exited) {
					partition.fs_resize_in_progress = false;
					reconfigure_runtime = true;
					device.rediscover();
					_reset_storage_dialog_operation();
				}
			}

		}); /* for each partition */

		/* respond to failure of part_block */
		if (device.discovery_in_progress()) {
			Child_exit_state exit_state(state, device.part_block_start_name());
			if (!exit_state.responsive) {
				error(device.part_block_start_name(), " got stuck");
				device.state = Storage_device::RELEASED;
				reconfigure_runtime = true;
			}
		}

		/* respond to completion of GPT relabeling */
		if (device.relabel_in_progress()) {
			Child_exit_state exit_state(state, device.relabel_start_name());
			if (exit_state.exited) {
				device.rediscover();
				reconfigure_runtime = true;
				_reset_storage_dialog_operation();
			}
		}

		/* respond to completion of GPT expand */
		if (device.gpt_expand_in_progress()) {
			Child_exit_state exit_state(state, device.expand_start_name());
			if (exit_state.exited) {

				/* kick off resize2fs */
				device.for_each_partition([&] (Partition &partition) {
					if (partition.gpt_expand_in_progress) {
						partition.gpt_expand_in_progress = false;
						partition.fs_resize_in_progress  = true;
					}
				});

				reconfigure_runtime = true;
				_reset_storage_dialog_operation();
			}
		}

	}); /* for each device */

	/* handle failed initialization of USB-storage devices */
	_storage._storage_devices.usb_storage_devices.for_each([&] (Usb_storage_device &dev) {
		String<64> name(dev.usb_block_drv_name());
		Child_exit_state exit_state(state, name);
		if (exit_state.exited) {
			dev.discard_usb_block_drv();
			reconfigure_runtime = true;
			regenerate_dialog   = true;
		}
	});

	/* remove prepare subsystem when finished */
	{
		Child_exit_state exit_state(state, "prepare");
		if (exit_state.exited) {
			_prepare_completed = _prepare_version;

			/* trigger update and deploy */
			reconfigure_runtime = true;
		}
	}

	/* schedule pending file operations to new fs_tool instance */
	{
		Child_exit_state exit_state(state, "fs_tool");

		if (exit_state.exited) {

			Child_exit_state::Version const expected_version(_fs_tool_version.value);

			if (exit_state.version == expected_version) {

				_file_operation_queue.schedule_next_operations();
				_fs_tool_version.value++;
				reconfigure_runtime = true;

				/* try to proceed after the first step of an depot-index update */
				unsigned const orig_download_count = _index_update_queue.download_count;
				_index_update_queue.try_schedule_downloads();
				if (_index_update_queue.download_count != orig_download_count)
					_deploy.update_installation();

				/* update depot-user selection after adding new depot URL */
				if (_depot_user_selection_visible())
					trigger_depot_query();
			}
		}
	}

	/* upgrade RAM and cap quota on demand */
	state.for_each_sub_node("child", [&] (Xml_node child) {

		bool reconfiguration_needed = false;
		_child_states.for_each([&] (Child_state &child_state) {
			if (child_state.apply_child_state_report(child))
				reconfiguration_needed = true; });

		if (reconfiguration_needed) {
			reconfigure_runtime = true;
			regenerate_dialog   = true;
		}
	});

	if (_deploy.update_child_conditions()) {
		reconfigure_runtime = true;
		regenerate_dialog   = true;
	}

	if (_software_section_dialog.selected() && _software_tabs_dialog.dialog.options_selected())
		regenerate_dialog = true;

	if (regenerate_dialog)
		generate_dialog();

	if (reconfigure_runtime)
		generate_runtime_config();
}


void Sculpt::Main::_generate_runtime_config(Xml_generator &xml) const
{
	xml.attribute("verbose", "yes");

	xml.attribute("prio_levels", _prio_levels.value);

	xml.node("report", [&] () {
		xml.attribute("init_ram",   "yes");
		xml.attribute("init_caps",  "yes");
		xml.attribute("child_ram",  "yes");
		xml.attribute("child_caps", "yes");
		xml.attribute("delay_ms",   4*500);
		xml.attribute("buffer",     "1M");
	});

	xml.node("heartbeat", [&] () { xml.attribute("rate_ms", 2000); });

	xml.node("parent-provides", [&] () {
		gen_parent_service<Rom_session>(xml);
		gen_parent_service<Cpu_session>(xml);
		gen_parent_service<Pd_session>(xml);
		gen_parent_service<Rm_session>(xml);
		gen_parent_service<Log_session>(xml);
		gen_parent_service<Vm_session>(xml);
		gen_parent_service<Timer::Session>(xml);
		gen_parent_service<Report::Session>(xml);
		gen_parent_service<Platform::Session>(xml);
		gen_parent_service<Block::Session>(xml);
		gen_parent_service<Usb::Session>(xml);
		gen_parent_service<::File_system::Session>(xml);
		gen_parent_service<Gui::Session>(xml);
		gen_parent_service<Rtc::Session>(xml);
		gen_parent_service<Trace::Session>(xml);
		gen_parent_service<Io_mem_session>(xml);
		gen_parent_service<Io_port_session>(xml);
		gen_parent_service<Irq_session>(xml);
		gen_parent_service<Event::Session>(xml);
		gen_parent_service<Capture::Session>(xml);
		gen_parent_service<Gpu::Session>(xml);
		gen_parent_service<Pin_state::Session>(xml);
		gen_parent_service<Pin_control::Session>(xml);
	});

	xml.node("affinity-space", [&] () {
		xml.attribute("width",  _affinity_space.width());
		xml.attribute("height", _affinity_space.height());
	});

	_main_menu_view.gen_start_node(xml);

	_touch_keyboard.gen_start_node(xml);

	_storage.gen_runtime_start_nodes(xml);

	/*
	 * Load configuration and update depot config on the sculpt partition
	 */
	if (_storage._sculpt_partition.valid() && _prepare_in_progress())
		xml.node("start", [&] () {
			gen_prepare_start_content(xml, _prepare_version); });

	/*
	 * Spawn chroot instances for accessing '/depot' and '/public'. The
	 * chroot instances implicitly refer to the 'default_fs_rw'.
	 */
	if (_storage._sculpt_partition.valid()) {

		auto chroot = [&] (Start_name const &name, Path const &path, Writeable w) {
			xml.node("start", [&] () {
				gen_chroot_start_content(xml, name, path, w); }); };

		if (_update_running()) {
			chroot("depot_rw",  "/depot",  WRITEABLE);
			chroot("public_rw", "/public", WRITEABLE);
		}

		chroot("depot", "/depot",  READ_ONLY);
	}

	/* execute file operations */
	if (_storage._sculpt_partition.valid())
		if (_file_operation_queue.any_operation_in_progress())
			xml.node("start", [&] () {
				gen_fs_tool_start_content(xml, _fs_tool_version,
				                          _file_operation_queue); });

	_network.gen_runtime_start_nodes(xml);

	if (_update_running())
		xml.node("start", [&] () {
			gen_update_start_content(xml); });

	if (_storage._sculpt_partition.valid() && !_prepare_in_progress()) {
		xml.node("start", [&] () {
			gen_launcher_query_start_content(xml); });

		_deploy.gen_runtime_start_nodes(xml, _prio_levels, _affinity_space);
	}
}


void Component::construct(Genode::Env &env)
{
	static Sculpt::Main main(env);
}

