extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_open_profiles(
    const epocanva_backend_profile_options* options, epocanva_backend_handle* out) noexcept {
  return Guard([&]() -> epocanva_status {
    if (!out) return EPOCANVA_INVALID_ARGUMENT;
    *out = 0;
    auto status = Structure(options, EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
    if (status != EPOCANVA_OK) return status;
    if (!options->profiles || !options->profile_count || options->profile_count > 32 || options->allow_test_fixture > 1)
      return EPOCANVA_INVALID_ARGUMENT;
    auto backend = std::make_unique<Backend>(); backend->profiles_only = true;
    for (std::uint32_t ordinal = 0; ordinal < options->profile_count; ++ordinal) {
      const auto& input = options->profiles[ordinal];
      Profile profile;
      status = ReadProfile(&input.profile, &profile);
      if (status != EPOCANVA_OK) return status;
      if (profile.language != input.resource.language || backend->profiles.count(profile.id)) return EPOCANVA_BACKEND_INVALID_PROFILE;
      status = Structure(&input.resource);
      if (status != EPOCANVA_OK) return status;
      if (input.resource.reserved) return EPOCANVA_INVALID_ARGUMENT;
      const auto identity = ResourceIdentity(input.resource);
      std::shared_ptr<Resource> resource;
      for (const auto& bound : backend->profile_resources)
        if (bound.second->identity == identity) { resource = bound.second; break; }
      if (!resource) {
        resource = std::make_shared<Resource>();
        status = LoadResource(input.resource, options->allow_test_fixture != 0, resource.get());
        if (status != EPOCANVA_OK) return status;
      }
      for (const auto& bound : backend->profiles) {
        if (backend->profile_resources.at(bound.first) == resource &&
            bound.second.abbreviation_path == profile.abbreviation_path &&
            bound.second.abbreviation_sha256 == profile.abbreviation_sha256) {
          profile.abbreviations = bound.second.abbreviations;
          break;
        }
      }
      if (!profile.abbreviations) {
        status = PrepareProfile(&profile, *resource);
        if (status != EPOCANVA_OK) return status;
      }
      backend->profile_resources.emplace(profile.id, std::move(resource));
      backend->profiles.emplace(profile.id, std::move(profile));
    }
    const auto root = Text(options->learning_root, 30000);
    if (!root.empty()) backend->repository = std::make_unique<learn::WindowsLearningRepository>(std::filesystem::u8path(root));
    if (options->rime_options) {
      status = epocanva_engine_open(options->rime_options, &backend->rime);
      if (status != EPOCANVA_OK) return status;
    }
    auto& registry = State(); std::lock_guard<std::mutex> lock(registry.mutex);
    const auto token = registry.Token(); if (!token) return EPOCANVA_OUT_OF_RANGE;
    registry.backends.emplace(token, std::move(backend)); *out = token;
    return EPOCANVA_OK;
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_profile_session_open(
    epocanva_backend_handle handle, const epocanva_backend_profile* value,
    const epocanva_backend_session_options* options, epocanva_backend_session_handle* out) noexcept {
  return Guard([&]() -> epocanva_status {
    if (!out) return EPOCANVA_INVALID_ARGUMENT;
    *out = 0;
    auto status = Structure(options); if (status != EPOCANVA_OK) return status;
    if (options->context.privacy_class != EPOCANVA_PRIVACY_NORMAL) return EPOCANVA_PRIVACY_DENIED;
    if (options->reserved || options->context.reserved || options->learning_enabled > 1 ||
        !options->context.context_target || !options->context.generation) return EPOCANVA_INVALID_ARGUMENT;
    Profile profile; status = ReadProfile(value, &profile); if (status != EPOCANVA_OK) return status;
    if (profile.language != options->language || profile.input_mode != options->input_mode) return EPOCANVA_BACKEND_INVALID_PROFILE;
    auto& registry = State(); std::lock_guard<std::mutex> lock(registry.mutex);
    auto found = registry.backends.find(handle); if (found == registry.backends.end()) return EPOCANVA_INVALID_HANDLE;
    auto& backend = *found->second;
    if (backend.owner != std::this_thread::get_id()) return EPOCANVA_WRONG_THREAD;
    const auto known = backend.profiles.find(profile.id);
    Resource* resource = nullptr;
    if (known != backend.profiles.end()) {
      if (!SameProfile(known->second, profile)) return EPOCANVA_BACKEND_INVALID_PROFILE;
      profile = known->second;
      const auto binding = backend.profile_resources.find(profile.id);
      if (binding != backend.profile_resources.end()) resource = binding->second.get();
      else resource = &backend.resources.at(profile.language);
      return RegisterSession(backend, *resource, std::move(profile), *options, out);
    } else {
      if (backend.profiles_only) return EPOCANVA_BACKEND_UNAVAILABLE;
      const auto binding = backend.resources.find(profile.language);
      if (binding == backend.resources.end()) return EPOCANVA_BACKEND_UNAVAILABLE;
      resource = &binding->second;
      status = PrepareProfile(&profile, *resource); if (status != EPOCANVA_OK) return status;
      const auto inserted = backend.profiles.emplace(profile.id, profile);
      try {
        status = RegisterSession(backend, *resource, std::move(profile), *options, out);
      } catch (...) {
        backend.profiles.erase(inserted.first);
        throw;
      }
      if (status != EPOCANVA_OK) backend.profiles.erase(inserted.first);
      return status;
    }
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_push_utf8(
    epocanva_backend_session_handle handle, const epocanva_backend_request* request, epocanva_utf8_view text) noexcept {
  return Guard([&]() -> epocanva_status {
    if ((!text.data && text.utf8_byte_count) || !text.utf8_byte_count) return EPOCANVA_INVALID_ARGUMENT;
    if (text.utf8_byte_count > kMaxInputBytes) return EPOCANVA_OUT_OF_RANGE;
    const std::string incoming(text.data, static_cast<std::size_t>(text.utf8_byte_count));
    if (!pred::IsValidUtf8(incoming)) return EPOCANVA_INVALID_UTF8;
    return With(handle, request, [&](Session& session, const learn::Access& access) -> epocanva_status {
      if (incoming.size() > kMaxInputBytes - session.raw.size()) return EPOCANVA_OUT_OF_RANGE;
      return Compose(session, session.raw + incoming, access);
    });
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_set_context(
    epocanva_backend_session_handle handle, const epocanva_backend_request* request, epocanva_utf8_view text) noexcept {
  return Guard([&]() -> epocanva_status {
    if ((!text.data && text.utf8_byte_count)) return EPOCANVA_INVALID_ARGUMENT;
    if (text.utf8_byte_count > kMaxInputBytes) return EPOCANVA_OUT_OF_RANGE;
    const std::string incoming = text.utf8_byte_count ? std::string(text.data, static_cast<std::size_t>(text.utf8_byte_count)) : std::string{};
    if (!pred::IsValidUtf8(incoming) || incoming.find('\0') != incoming.npos) return EPOCANVA_INVALID_UTF8;
    return With(handle, request, [&](Session& session, const learn::Access& access) {
      auto ids = epocanva::backend::ResolveContext(*session.resource->model, Language(session.language), incoming);
      return Result(session.flow->SetContext(std::move(ids), access));
    });
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_confirm_text(
    epocanva_backend_session_handle handle, const epocanva_backend_text_confirmation* confirmation,
    epocanva_backend_ack_result* out) noexcept {
  return Guard([&]() -> epocanva_status {
    auto status = Output(out); if (status != EPOCANVA_OK) return status;
    status = Structure(confirmation, EPOCANVA_BACKEND_PROFILE_ABI_VERSION); if (status != EPOCANVA_OK) return status;
    status = Request(&confirmation->request); if (status != EPOCANVA_OK) return status;
    if (!confirmation->event_token) return EPOCANVA_INVALID_ARGUMENT;
    auto& registry = State(); std::lock_guard<std::mutex> lock(registry.mutex);
    const auto found = registry.sessions.find(handle); if (found == registry.sessions.end()) return EPOCANVA_INVALID_HANDLE;
    auto& session = *found->second;
    if (session.backend->owner != std::this_thread::get_id()) return EPOCANVA_WRONG_THREAD;
    const auto access = Access(confirmation->request.context, confirmation->request.learning_enabled);
    status = Result(session.flow->Check(access));
    if (status == EPOCANVA_PRIVACY_DENIED) {
      const auto cleared = Result(session.flow->Reset(access));
      if (cleared != EPOCANVA_OK) return cleared;
      session.raw.clear(); session.preedit.clear(); session.selected_context_ids.clear();
      session.japanese.Cancel(); session.korean.Cancel();
      if (session.rime) (void)RimeReset(session);
      session.confirmed_event_token = std::max(session.confirmed_event_token, confirmation->event_token);
      out->consumed = 1;
      return EPOCANVA_PRIVACY_DENIED;
    }
    if (status != EPOCANVA_OK) return status;
    if (confirmation->event_token <= session.confirmed_event_token) return EPOCANVA_STALE_SNAPSHOT;
    const auto view = confirmation->text;
    if ((!view.data && view.utf8_byte_count)) return EPOCANVA_INVALID_ARGUMENT;
    if (view.utf8_byte_count > kMaxInputBytes) return EPOCANVA_OUT_OF_RANGE;
    const std::string incoming = view.utf8_byte_count ? std::string(view.data, static_cast<std::size_t>(view.utf8_byte_count)) : std::string{};
    if (!pred::IsValidUtf8(incoming) || incoming.find('\0') != incoming.npos) return EPOCANVA_INVALID_UTF8;
    auto ids = epocanva::backend::ResolveContext(*session.resource->model, Language(session.language), incoming,
        session.flow->Context(access));
    const bool advanced = !ids.empty();
    status = Result(session.flow->SetContext(std::move(ids), access, false, true));
    if (status != EPOCANVA_OK) return status;
    session.confirmed_event_token = confirmation->event_token;
    out->consumed = 1; out->context_advanced = advanced;
    session.raw.clear(); session.preedit.clear(); session.japanese.Cancel(); session.korean.Cancel();
    if (session.rime) { status = RimeReset(session); if (status != EPOCANVA_OK) return status; }
    return EPOCANVA_OK;
  });
}
extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_backend_read_profile_snapshot(
    epocanva_backend_session_handle handle, const epocanva_backend_request* request,
    epocanva_backend_profile_snapshot* out, char* buffer, std::uint64_t capacity) noexcept {
  return Guard([&]() -> epocanva_status {
    auto status = Structure(out, EPOCANVA_BACKEND_PROFILE_ABI_VERSION); if (status != EPOCANVA_OK) return status;
    out->profile_id = 0; out->output_mode = 0; out->keyboard_layout = 0; out->unit_kind = EPOCANVA_BACKEND_UNIT_LEXICAL_TOKEN;
    status = epocanva_backend_read_snapshot(handle, request, &out->snapshot, buffer, capacity);
    if (status != EPOCANVA_OK && status != EPOCANVA_BUFFER_TOO_SMALL) return status;
    const auto copied_status = status;
    return With(handle, request, [&](Session& session, const learn::Access&) {
      out->profile_id = session.profile.id; out->output_mode = session.profile.output_mode;
      out->keyboard_layout = session.profile.keyboard_layout;
      return copied_status;
    });
  });
}
