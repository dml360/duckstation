#include "cheats.h"
#include "bus.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string.h"
#include "common/string_util.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "host_interface.h"
#include <cctype>
#include <iomanip>
#include <sstream>
Log_SetChannel(Cheats);

using KeyValuePairVector = std::vector<std::pair<std::string, std::string>>;

static bool IsValidScanAddress(PhysicalMemoryAddress address)
{
  if ((address & CPU::DCACHE_LOCATION_MASK) == CPU::DCACHE_LOCATION &&
      (address & CPU::DCACHE_OFFSET_MASK) < CPU::DCACHE_SIZE)
  {
    return true;
  }

  address &= CPU::PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < Bus::RAM_MIRROR_END)
    return true;

  if (address >= Bus::BIOS_BASE && address < (Bus::BIOS_BASE + Bus::BIOS_SIZE))
    return true;

  return false;
}

template<typename T>
static T DoMemoryRead(PhysicalMemoryAddress address)
{
  T result;

  if ((address & CPU::DCACHE_LOCATION_MASK) == CPU::DCACHE_LOCATION &&
      (address & CPU::DCACHE_OFFSET_MASK) < CPU::DCACHE_SIZE)
  {
    std::memcpy(&result, &CPU::g_state.dcache[address & CPU::DCACHE_OFFSET_MASK], sizeof(result));
    return result;
  }

  address &= CPU::PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < Bus::RAM_MIRROR_END)
  {
    std::memcpy(&result, &Bus::g_ram[address & Bus::RAM_MASK], sizeof(result));
    return result;
  }

  if (address >= Bus::BIOS_BASE && address < (Bus::BIOS_BASE + Bus::BIOS_SIZE))
  {
    std::memcpy(&result, &Bus::g_bios[address & Bus::BIOS_MASK], sizeof(result));
    return result;
  }

  result = static_cast<T>(0);
  return result;
}

template<typename T>
static void DoMemoryWrite(PhysicalMemoryAddress address, T value)
{
  if ((address & CPU::DCACHE_LOCATION_MASK) == CPU::DCACHE_LOCATION &&
      (address & CPU::DCACHE_OFFSET_MASK) < CPU::DCACHE_SIZE)
  {
    std::memcpy(&CPU::g_state.dcache[address & CPU::DCACHE_OFFSET_MASK], &value, sizeof(value));
    return;
  }

  address &= CPU::PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < Bus::RAM_MIRROR_END)
  {
    // Only invalidate code when it changes.
    T old_value;
    std::memcpy(&old_value, &Bus::g_ram[address & Bus::RAM_MASK], sizeof(old_value));
    if (old_value != value)
    {
      std::memcpy(&Bus::g_ram[address & Bus::RAM_MASK], &value, sizeof(value));

      const u32 code_page_index = Bus::GetRAMCodePageIndex(address & Bus::RAM_MASK);
      if (Bus::IsRAMCodePage(code_page_index))
        CPU::CodeCache::InvalidateBlocksWithPageIndex(code_page_index);
    }

    return;
  }
}

CheatList::CheatList() = default;

CheatList::~CheatList() = default;

static bool IsHexCharacter(char c)
{
  return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
}

static const std::string* FindKey(const KeyValuePairVector& kvp, const char* search)
{
  for (const auto& it : kvp)
  {
    if (StringUtil::Strcasecmp(it.first.c_str(), search) == 0)
      return &it.second;
  }

  return nullptr;
}

bool CheatList::LoadFromPCSXRFile(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
    return false;

  char line[1024];
  std::string comments;
  std::string group;
  CheatCode::Type type = CheatCode::Type::Gameshark;
  CheatCode::Activation activation = CheatCode::Activation::EndFrame;
  CheatCode current_code;
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    char* start = line;
    while (*start != '\0' && std::isspace(*start))
      start++;

    // skip empty lines
    if (*start == '\0')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(*end))
    {
      *end = '\0';
      end--;
    }

    // DuckStation metadata
    if (StringUtil::Strncasecmp(start, "#group=", 7) == 0)
    {
      group = start + 7;
      continue;
    }
    if (StringUtil::Strncasecmp(start, "#type=", 6) == 0)
    {
      type = CheatCode::ParseTypeName(start + 6).value_or(CheatCode::Type::Gameshark);
      continue;
    }
    if (StringUtil::Strncasecmp(start, "#activation=", 12) == 0)
    {
      activation = CheatCode::ParseActivationName(start + 12).value_or(CheatCode::Activation::EndFrame);
      continue;
    }

    // skip comments and empty line
    if (*start == '#' || *start == ';' || *start == '/' || *start == '\"')
    {
      comments.append(start);
      comments += '\n';
      continue;
    }

    if (*start == '[' && *end == ']')
    {
      start++;
      *end = '\0';

      // new cheat
      if (current_code.Valid())
        m_codes.push_back(std::move(current_code));

      current_code = CheatCode();
      if (group.empty())
        group = "Ungrouped";

      current_code.group = std::move(group);
      group = std::string();
      current_code.comments = std::move(comments);
      comments = std::string();
      current_code.type = type;
      type = CheatCode::Type::Gameshark;
      current_code.activation = activation;
      activation = CheatCode::Activation::EndFrame;

      if (*start == '*')
      {
        current_code.enabled = true;
        start++;
      }

      current_code.description.append(start);
      continue;
    }

    while (!IsHexCharacter(*start) && start != end)
      start++;
    if (start == end)
      continue;

    char* end_ptr;
    CheatCode::Instruction inst;
    inst.first = static_cast<u32>(std::strtoul(start, &end_ptr, 16));
    inst.second = 0;
    if (end_ptr)
    {
      while (!IsHexCharacter(*end_ptr) && end_ptr != end)
        end_ptr++;
      if (end_ptr != end)
        inst.second = static_cast<u32>(std::strtoul(end_ptr, nullptr, 16));
    }
    current_code.instructions.push_back(inst);
  }

  if (current_code.Valid())
  {
    // technically this isn't the place for end of file
    if (!comments.empty())
      current_code.comments += comments;
    m_codes.push_back(std::move(current_code));
  }

  Log_InfoPrintf("Loaded %zu cheats from '%s' (PCSXR format)", m_codes.size(), filename);
  return !m_codes.empty();
}

bool CheatList::LoadFromLibretroFile(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
    return false;

  char line[1024];
  KeyValuePairVector kvp;
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    char* start = line;
    while (*start != '\0' && std::isspace(*start))
      start++;

    // skip empty lines
    if (*start == '\0' || *start == '=')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(*end))
    {
      *end = '\0';
      end--;
    }

    char* equals = start;
    while (*equals != '=' && equals != end)
      equals++;
    if (equals == end)
      continue;

    *equals = '\0';

    char* key_end = equals - 1;
    while (key_end > start && std::isspace(*key_end))
    {
      *key_end = '\0';
      key_end--;
    }

    char* value_start = equals + 1;
    while (*value_start != '\0' && std::isspace(*value_start))
      value_start++;

    if (*value_start == '\0')
      continue;

    char* value_end = value_start + std::strlen(value_start) - 1;
    while (value_end > value_start && std::isspace(*value_end))
    {
      *value_end = '\0';
      value_end--;
    }

    if (*value_start == '\"')
    {
      if (*value_end != '\"')
        continue;

      value_start++;
      *value_end = '\0';
    }

    kvp.emplace_back(start, value_start);
  }

  if (kvp.empty())
    return false;

  const std::string* num_cheats_value = FindKey(kvp, "cheats");
  const u32 num_cheats = num_cheats_value ? StringUtil::FromChars<u32>(*num_cheats_value).value_or(0) : 0;
  if (num_cheats == 0)
    return false;

  for (u32 i = 0; i < num_cheats; i++)
  {
    const std::string* desc = FindKey(kvp, TinyString::FromFormat("cheat%u_desc", i));
    const std::string* code = FindKey(kvp, TinyString::FromFormat("cheat%u_code", i));
    const std::string* enable = FindKey(kvp, TinyString::FromFormat("cheat%u_enable", i));
    if (!desc || !code || !enable)
    {
      Log_WarningPrintf("Missing desc/code/enable for cheat %u in '%s'", i, filename);
      continue;
    }

    CheatCode cc;
    cc.group = "Ungrouped";
    cc.description = *desc;
    cc.enabled = StringUtil::FromChars<bool>(*enable).value_or(false);
    if (ParseLibretroCheat(&cc, code->c_str()))
      m_codes.push_back(std::move(cc));
  }

  Log_InfoPrintf("Loaded %zu cheats from '%s' (libretro format)", m_codes.size(), filename);
  return !m_codes.empty();
}

static bool IsLibretroSeparator(char ch)
{
  return (ch == ' ' || ch == '-' || ch == ':' || ch == '+');
}

bool CheatList::ParseLibretroCheat(CheatCode* cc, const char* line)
{
  const char* current_ptr = line;
  while (current_ptr)
  {
    char* end_ptr;
    CheatCode::Instruction inst;
    inst.first = static_cast<u32>(std::strtoul(current_ptr, &end_ptr, 16));
    current_ptr = end_ptr;
    if (end_ptr)
    {
      if (!IsLibretroSeparator(*end_ptr))
      {
        Log_WarningPrintf("Malformed code '%s'", line);
        break;
      }

      end_ptr++;
      inst.second = static_cast<u32>(std::strtoul(current_ptr, &end_ptr, 16));
      if (end_ptr && *end_ptr == '\0')
        end_ptr = nullptr;

      if (end_ptr && *end_ptr != '\0')
      {
        if (!IsLibretroSeparator(*end_ptr))
        {
          Log_WarningPrintf("Malformed code '%s'", line);
          break;
        }

        end_ptr++;
      }

      current_ptr = end_ptr;
      cc->instructions.push_back(inst);
    }
  }

  return !cc->instructions.empty();
}

void CheatList::Apply()
{
  for (const CheatCode& code : m_codes)
  {
    if (code.enabled)
      code.Apply();
  }
}

void CheatList::AddCode(CheatCode cc)
{
  m_codes.push_back(std::move(cc));
}

void CheatList::SetCode(u32 index, CheatCode cc)
{
  if (index > m_codes.size())
    return;

  if (index == m_codes.size())
  {
    m_codes.push_back(std::move(cc));
    return;
  }

  m_codes[index] = std::move(cc);
}

void CheatList::RemoveCode(u32 i)
{
  m_codes.erase(m_codes.begin() + i);
}

std::optional<CheatList::Format> CheatList::DetectFileFormat(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
    return Format::Count;

  char line[1024];
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    char* start = line;
    while (*start != '\0' && std::isspace(*start))
      start++;

    // skip empty lines
    if (*start == '\0' || *start == '=')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(*end))
    {
      *end = '\0';
      end--;
    }

    if (std::strncmp(line, "cheats", 6) == 0)
      return Format::Libretro;
    else
      return Format::PCSXR;
  }

  return Format::Count;
}

bool CheatList::LoadFromFile(const char* filename, Format format)
{
  if (format == Format::Autodetect)
    format = DetectFileFormat(filename).value_or(Format::Count);

  if (format == Format::PCSXR)
    return LoadFromPCSXRFile(filename);
  else if (format == Format::Libretro)
    return LoadFromLibretroFile(filename);

  Log_ErrorPrintf("Invalid or unknown format for '%s'", filename);
  return false;
}

bool CheatList::SaveToPCSXRFile(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "wb");
  if (!fp)
    return false;

  for (const CheatCode& cc : m_codes)
  {
    if (!cc.comments.empty())
      std::fputs(cc.comments.c_str(), fp.get());
    std::fprintf(fp.get(), "#group=%s\n", cc.group.c_str());
    std::fprintf(fp.get(), "#type=%s\n", CheatCode::GetTypeName(cc.type));
    std::fprintf(fp.get(), "#activation=%s\n", CheatCode::GetActivationName(cc.activation));
    std::fprintf(fp.get(), "[%s%s]\n", cc.enabled ? "*" : "", cc.description.c_str());
    for (const CheatCode::Instruction& i : cc.instructions)
      std::fprintf(fp.get(), "%08X %04X\n", i.first, i.second);
    std::fprintf(fp.get(), "\n");
  }

  std::fflush(fp.get());
  return (std::ferror(fp.get()) == 0);
}

u32 CheatList::GetEnabledCodeCount() const
{
  u32 count = 0;
  for (const CheatCode& cc : m_codes)
  {
    if (cc.enabled)
      count++;
  }

  return count;
}

std::vector<std::string> CheatList::GetCodeGroups() const
{
  std::vector<std::string> groups;
  for (const CheatCode& cc : m_codes)
  {
    if (std::any_of(groups.begin(), groups.end(), [cc](const std::string& group) { return (group == cc.group); }))
      continue;

    groups.emplace_back(cc.group);
  }

  return groups;
}

void CheatList::SetCodeEnabled(u32 index, bool state)
{
  if (index >= m_codes.size())
    return;

  m_codes[index].enabled = state;
}

void CheatList::EnableCode(u32 index)
{
  SetCodeEnabled(index, true);
}

void CheatList::DisableCode(u32 index)
{
  SetCodeEnabled(index, false);
}

void CheatList::ApplyCode(u32 index)
{
  if (index >= m_codes.size())
    return;

  m_codes[index].Apply();
}

std::string CheatCode::GetInstructionsAsString() const
{
  std::stringstream ss;

  for (const Instruction& inst : instructions)
  {
    ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << inst.first;
    ss << " ";
    ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << inst.second;
    ss << '\n';
  }

  return ss.str();
}

bool CheatCode::SetInstructionsFromString(const std::string& str)
{
  std::vector<Instruction> new_instructions;
  std::istringstream ss(str);

  for (std::string line; std::getline(ss, line);)
  {
    char* start = line.data();
    while (*start != '\0' && std::isspace(*start))
      start++;

    // skip empty lines
    if (*start == '\0')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(*end))
    {
      *end = '\0';
      end--;
    }

    // skip comments and empty line
    if (*start == '#' || *start == ';' || *start == '/' || *start == '\"')
      continue;

    while (!IsHexCharacter(*start) && start != end)
      start++;
    if (start == end)
      continue;

    char* end_ptr;
    CheatCode::Instruction inst;
    inst.first = static_cast<u32>(std::strtoul(start, &end_ptr, 16));
    inst.second = 0;
    if (end_ptr)
    {
      while (!IsHexCharacter(*end_ptr) && end_ptr != end)
        end_ptr++;
      if (end_ptr != end)
        inst.second = static_cast<u32>(std::strtoul(end_ptr, nullptr, 16));
    }
    new_instructions.push_back(inst);
  }

  if (new_instructions.empty())
    return false;

  instructions = std::move(new_instructions);
  return true;
}

void CheatCode::Apply() const
{
  const u32 count = static_cast<u32>(instructions.size());
  u32 index = 0;
  for (; index < count;)
  {
    const Instruction& inst = instructions[index];
    switch (inst.code)
    {
      case InstructionCode::Nop:
      {
        index++;
      }
      break;

      case InstructionCode::ConstantWrite8:
      {
        DoMemoryWrite<u8>(inst.address, inst.value8);
        index++;
      }
      break;

      case InstructionCode::ConstantWrite16:
      {
        DoMemoryWrite<u16>(inst.address, inst.value16);
        index++;
      }
      break;

      case InstructionCode::ScratchpadWrite16:
      {
        DoMemoryWrite<u16>(CPU::DCACHE_LOCATION | (inst.address & CPU::DCACHE_OFFSET_MASK), inst.value16);
        index++;
      }
      break;

      case InstructionCode::Increment16:
      {
        u16 value = DoMemoryRead<u16>(inst.address);
        DoMemoryWrite<u16>(inst.address, value + inst.value16);
        index++;
      }
      break;

      case InstructionCode::Decrement16:
      {
        u16 value = DoMemoryRead<u16>(inst.address);
        DoMemoryWrite<u16>(inst.address, value - inst.value16);
        index++;
      }
      break;

      case InstructionCode::Increment8:
      {
        u8 value = DoMemoryRead<u8>(inst.address);
        DoMemoryWrite<u8>(inst.address, value + inst.value8);
        index++;
      }
      break;

      case InstructionCode::Decrement8:
      {
        u8 value = DoMemoryRead<u8>(inst.address);
        DoMemoryWrite<u8>(inst.address, value - inst.value8);
        index++;
      }
      break;

      case InstructionCode::CompareEqual16:
      {
        u16 value = DoMemoryRead<u16>(inst.address);
        if (value == inst.value16)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareNotEqual16:
      {
        u16 value = DoMemoryRead<u16>(inst.address);
        if (value != inst.value16)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareLess16:
      {
        u16 value = DoMemoryRead<u16>(inst.address);
        if (value < inst.value16)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareGreater16:
      {
        u16 value = DoMemoryRead<u16>(inst.address);
        if (value > inst.value16)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareEqual8:
      {
        u8 value = DoMemoryRead<u8>(inst.address);
        if (value == inst.value8)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareNotEqual8:
      {
        u8 value = DoMemoryRead<u8>(inst.address);
        if (value != inst.value8)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareLess8:
      {
        u8 value = DoMemoryRead<u8>(inst.address);
        if (value < inst.value8)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareGreater8:
      {
        u8 value = DoMemoryRead<u8>(inst.address);
        if (value > inst.value8)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::Slide:
      {
        if ((index + 1) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete slide instruction");
          return;
        }

        const u32 slide_count = (inst.first >> 8) & 0xFFu;
        const u32 address_increment = SignExtendN<8>(inst.first & 0xFFu);
        const u16 value_increment = SignExtendN<8>(Truncate16(inst.second & 0xFFu));
        const Instruction& inst2 = instructions[index + 1];
        const InstructionCode write_type = inst2.code;
        u32 address = inst2.address;
        u16 value = inst2.value16;

        if (write_type == InstructionCode::ConstantWrite8)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u8>(address, Truncate8(value));
            address += address_increment;
            value += value_increment;
          }
        }
        else if (write_type == InstructionCode::ConstantWrite16)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u16>(address, value);
            address += address_increment;
            value += value_increment;
          }
        }
        else
        {
          Log_ErrorPrintf("Invalid command in second slide parameter 0x%02X", write_type);
        }

        index += 2;
      }
      break;

      case InstructionCode::MemoryCopy:
      {
        if ((index + 1) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete memory copy instruction");
          return;
        }

        const Instruction& inst2 = instructions[index + 1];
        const u32 byte_count = inst.value16;
        u32 src_address = inst.address;
        u32 dst_address = inst2.address;

        for (u32 i = 0; i < byte_count; i++)
        {
          u8 value = DoMemoryRead<u8>(src_address);
          DoMemoryWrite<u8>(dst_address, value);
          src_address++;
          dst_address++;
        }

        index += 2;
      }
      break;

      default:
      {
        Log_ErrorPrintf("Unhandled instruction code 0x%02X (%08X %08X)", static_cast<u8>(inst.code.GetValue()),
                        inst.first, inst.second);
        index++;
      }
      break;
    }
  }
}

static std::array<const char*, 1> s_cheat_code_type_names = {{"Gameshark"}};
static std::array<const char*, 1> s_cheat_code_type_display_names{{TRANSLATABLE("Cheats", "Gameshark")}};

const char* CheatCode::GetTypeName(Type type)
{
  return s_cheat_code_type_names[static_cast<u32>(type)];
}

const char* CheatCode::GetTypeDisplayName(Type type)
{
  return s_cheat_code_type_display_names[static_cast<u32>(type)];
}

std::optional<CheatCode::Type> CheatCode::ParseTypeName(const char* str)
{
  for (u32 i = 0; i < static_cast<u32>(s_cheat_code_type_names.size()); i++)
  {
    if (std::strcmp(s_cheat_code_type_names[i], str) == 0)
      return static_cast<Type>(i);
  }

  return std::nullopt;
}

static std::array<const char*, 2> s_cheat_code_activation_names = {{"Manual", "EndFrame"}};
static std::array<const char*, 2> s_cheat_code_activation_display_names{
  {TRANSLATABLE("Cheats", "Manual"), TRANSLATABLE("Cheats", "Automatic (Frame End)")}};

const char* CheatCode::GetActivationName(Activation activation)
{
  return s_cheat_code_activation_names[static_cast<u32>(activation)];
}

const char* CheatCode::GetActivationDisplayName(Activation activation)
{
  return s_cheat_code_activation_display_names[static_cast<u32>(activation)];
}

std::optional<CheatCode::Activation> CheatCode::ParseActivationName(const char* str)
{
  for (u32 i = 0; i < static_cast<u32>(s_cheat_code_activation_names.size()); i++)
  {
    if (std::strcmp(s_cheat_code_activation_names[i], str) == 0)
      return static_cast<Activation>(i);
  }

  return std::nullopt;
}

MemoryScan::MemoryScan() = default;

MemoryScan::~MemoryScan() = default;

void MemoryScan::ResetSearch()
{
  m_results.clear();
}

void MemoryScan::Search()
{
  m_results.clear();

  switch (m_size)
  {
    case MemoryAccessSize::Byte:
      SearchBytes();
      break;

    case MemoryAccessSize::HalfWord:
      SearchHalfwords();
      break;

    case MemoryAccessSize::Word:
      SearchWords();
      break;

    default:
      break;
  }
}

void MemoryScan::SearchBytes()
{
  for (PhysicalMemoryAddress address = m_start_address; address < m_end_address; address++)
  {
    if (!IsValidScanAddress(address))
      continue;

    const u8 bvalue = DoMemoryRead<u8>(address);

    Result res;
    res.address = address;
    res.value = m_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    res.last_value = res.value;
    res.value_changed = false;

    if (res.Filter(m_operator, m_value, m_signed))
      m_results.push_back(res);
  }
}

void MemoryScan::SearchHalfwords()
{
  for (PhysicalMemoryAddress address = m_start_address; address < m_end_address; address += 2)
  {
    if (!IsValidScanAddress(address))
      continue;

    const u16 bvalue = DoMemoryRead<u16>(address);

    Result res;
    res.address = address;
    res.value = m_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    res.last_value = res.value;
    res.value_changed = false;

    if (res.Filter(m_operator, m_value, m_signed))
      m_results.push_back(res);
  }
}

void MemoryScan::SearchWords()
{
  for (PhysicalMemoryAddress address = m_start_address; address < m_end_address; address += 4)
  {
    if (!IsValidScanAddress(address))
      continue;

    Result res;
    res.address = address;
    res.value = DoMemoryRead<u32>(address);
    res.last_value = res.value;
    res.value_changed = false;

    if (res.Filter(m_operator, m_value, m_signed))
      m_results.push_back(res);
  }
}

void MemoryScan::SearchAgain()
{
  ResultVector new_results;
  new_results.reserve(m_results.size());
  for (Result& res : m_results)
  {
    res.UpdateValue(m_size, m_signed);

    if (res.Filter(m_operator, m_value, m_signed))
    {
      res.last_value = res.value;
      new_results.push_back(res);
    }
  }

  m_results.swap(new_results);
}

void MemoryScan::UpdateResultsValues()
{
  for (Result& res : m_results)
    res.UpdateValue(m_size, m_signed);
}

void MemoryScan::SetResultValue(u32 index, u32 value)
{
  if (index >= m_results.size())
    return;

  Result& res = m_results[index];
  if (res.value == value)
    return;

  switch (m_size)
  {
    case MemoryAccessSize::Byte:
      DoMemoryWrite<u8>(res.address, Truncate8(value));
      break;

    case MemoryAccessSize::HalfWord:
      DoMemoryWrite<u16>(res.address, Truncate16(value));
      break;

    case MemoryAccessSize::Word:
      CPU::SafeWriteMemoryWord(res.address, value);
      break;
  }

  res.value = value;
  res.value_changed = true;
}

bool MemoryScan::Result::Filter(Operator op, u32 comp_value, bool is_signed) const
{
  switch (op)
  {
    case Operator::Equal:
    {
      return (value == comp_value);
    }

    case Operator::NotEqual:
    {
      return (value != comp_value);
    }

    case Operator::GreaterThan:
    {
      return is_signed ? (static_cast<s32>(value) > static_cast<s32>(comp_value)) : (value > comp_value);
    }

    case Operator::GreaterEqual:
    {
      return is_signed ? (static_cast<s32>(value) >= static_cast<s32>(comp_value)) : (value >= comp_value);
    }

    case Operator::LessThan:
    {
      return is_signed ? (static_cast<s32>(value) < static_cast<s32>(comp_value)) : (value < comp_value);
    }

    case Operator::LessEqual:
    {
      return is_signed ? (static_cast<s32>(value) <= static_cast<s32>(comp_value)) : (value <= comp_value);
    }

    case Operator::IncreasedBy:
    {
      return is_signed ? ((static_cast<s32>(value) - static_cast<s32>(last_value)) == static_cast<s32>(comp_value)) :
                         ((value - last_value) == comp_value);
    }

    case Operator::DecreasedBy:
    {
      return is_signed ? ((static_cast<s32>(last_value) - static_cast<s32>(value)) == static_cast<s32>(comp_value)) :
                         ((last_value - value) == comp_value);
    }

    case Operator::ChangedBy:
    {
      if (is_signed)
        return (std::abs(static_cast<s32>(last_value) - static_cast<s32>(value)) == static_cast<s32>(comp_value));
      else
        return ((last_value > value) ? (last_value - value) : (value - last_value)) == comp_value;
    }

    case Operator::EqualLast:
    {
      return (value == last_value);
    }

    case Operator::NotEqualLast:
    {
      return (value != last_value);
    }

    case Operator::GreaterThanLast:
    {
      return is_signed ? (static_cast<s32>(value) > static_cast<s32>(last_value)) : (value > last_value);
    }

    case Operator::GreaterEqualLast:
    {
      return is_signed ? (static_cast<s32>(value) >= static_cast<s32>(last_value)) : (value >= last_value);
    }

    case Operator::LessThanLast:
    {
      return is_signed ? (static_cast<s32>(value) < static_cast<s32>(last_value)) : (value < last_value);
    }

    case Operator::LessEqualLast:
    {
      return is_signed ? (static_cast<s32>(value) <= static_cast<s32>(last_value)) : (value <= last_value);
    }

    case Operator::Any:
      return true;

    default:
      return false;
  }
}

void MemoryScan::Result::UpdateValue(MemoryAccessSize size, bool is_signed)
{
  const u32 old_value = value;

  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      u8 bvalue = DoMemoryRead<u8>(address);
      value = is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::HalfWord:
    {
      u16 bvalue = DoMemoryRead<u16>(address);
      value = is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::Word:
    {
      CPU::SafeReadMemoryWord(address, &value);
    }
    break;
  }

  value_changed = (value != old_value);
}

MemoryWatchList::MemoryWatchList() = default;

MemoryWatchList::~MemoryWatchList() = default;

const MemoryWatchList::Entry* MemoryWatchList::GetEntryByAddress(u32 address) const
{
  for (const Entry& entry : m_entries)
  {
    if (entry.address == address)
      return &entry;
  }

  return nullptr;
}

bool MemoryWatchList::AddEntry(std::string description, u32 address, MemoryAccessSize size, bool is_signed, bool freeze)
{
  if (GetEntryByAddress(address))
    return false;

  Entry entry;
  entry.description = std::move(description);
  entry.address = address;
  entry.size = size;
  entry.is_signed = is_signed;
  entry.freeze = false;

  UpdateEntryValue(&entry);

  entry.changed = false;
  entry.freeze = freeze;

  m_entries.push_back(std::move(entry));
  return true;
}

void MemoryWatchList::RemoveEntry(u32 index)
{
  if (index >= m_entries.size())
    return;

  m_entries.erase(m_entries.begin() + index);
}

bool MemoryWatchList::RemoveEntryByAddress(u32 address)
{
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
  {
    if (it->address == address)
    {
      m_entries.erase(it);
      return true;
    }
  }

  return false;
}

void MemoryWatchList::SetEntryDescription(u32 index, std::string description)
{
  if (index >= m_entries.size())
    return;

  Entry& entry = m_entries[index];
  entry.description = std::move(description);
}

void MemoryWatchList::SetEntryFreeze(u32 index, bool freeze)
{
  if (index >= m_entries.size())
    return;

  Entry& entry = m_entries[index];
  entry.freeze = freeze;
}

void MemoryWatchList::SetEntryValue(u32 index, u32 value)
{
  if (index >= m_entries.size())
    return;

  Entry& entry = m_entries[index];
  if (entry.value == value)
    return;

  SetEntryValue(&entry, value);
}

bool MemoryWatchList::RemoveEntryByDescription(const char* description)
{
  bool result = false;
  for (auto it = m_entries.begin(); it != m_entries.end();)
  {
    if (it->description == description)
    {
      it = m_entries.erase(it);
      result = true;
      continue;
    }

    ++it;
  }

  return result;
}

void MemoryWatchList::UpdateValues()
{
  for (Entry& entry : m_entries)
    UpdateEntryValue(&entry);
}

void MemoryWatchList::SetEntryValue(Entry* entry, u32 value)
{
  switch (entry->size)
  {
    case MemoryAccessSize::Byte:
      DoMemoryWrite<u8>(entry->address, Truncate8(value));
      break;

    case MemoryAccessSize::HalfWord:
      DoMemoryWrite<u16>(entry->address, Truncate16(value));
      break;

    case MemoryAccessSize::Word:
      DoMemoryWrite<u32>(entry->address, value);
      break;
  }

  entry->changed = (entry->value != value);
  entry->value = value;
}

void MemoryWatchList::UpdateEntryValue(Entry* entry)
{
  const u32 old_value = entry->value;

  switch (entry->size)
  {
    case MemoryAccessSize::Byte:
    {
      u8 bvalue = DoMemoryRead<u8>(entry->address);
      entry->value = entry->is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::HalfWord:
    {
      u16 bvalue = DoMemoryRead<u16>(entry->address);
      entry->value = entry->is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::Word:
    {
      entry->value = DoMemoryRead<u32>(entry->address);
    }
    break;
  }

  entry->changed = (old_value != entry->value);

  if (entry->freeze && entry->changed)
    SetEntryValue(entry, old_value);
}
