#include "JokeTool.h"
#include <random>
#include <chrono>
#include <algorithm>
#include <iostream>

namespace Jarvis {

std::vector<ToolParameter> JokeTool::getParameters() const {
    return {
        {"category", "string", "Joke category: 'general', 'programming', 'pun', or 'random' (default)", false, "random"}
    };
}

static const std::vector<std::string> programming_jokes = {
    "Why do programmers prefer dark mode? Because light attracts bugs!",
    "How many programmers does it take to change a light bulb? None, that's a hardware problem.",
    "A SQL query walks into a bar, walks up to two tables and asks: Can I join you?",
    "Why did the programmer quit their job? Because they didn't get arrays.",
    "There are 10 types of people: those who understand binary and those who don't.",
    "Why do Java programmers have to wear glasses? Because they don't C sharp!",
    "A programmer's wife tells him: Go to the store and get a gallon of milk. If they have eggs, get a dozen. He returns with 12 gallons of milk.",
    "Why was the developer unhappy at their job? They wanted arrays.",
    "Knock knock. Who's there? Recursion. Recursion who? Knock knock.",
    "I told a UDP joke once. You might not get it.",
    "Why do programmers confuse Christmas and Halloween? Oct 31 == Dec 25.",
    "Real programmers count from 0.",
    "A code without tests is like a joke without a punchline.",
    "Git commit -m 'fix' — the most honest commit message ever."
};

static const std::vector<std::string> general_jokes = {
    "Why don't scientists trust atoms? Because they make up everything!",
    "I told my wife she was drawing her eyebrows too high. She looked surprised.",
    "What do you call a fish without eyes? A fsh.",
    "Why can't you trust an atom? They make up literally everything.",
    "I'm reading a book about anti-gravity. It's impossible to put down.",
    "Did you hear about the mathematician who's afraid of negative numbers? He'll stop at nothing to avoid them.",
    "Why do cows wear bells? Because their horns don't work!",
    "What did the ocean say to the beach? Nothing, it just waved.",
    "I told my doctor that I broke my arm in two places. He said stop going to those places.",
    "Time flies like an arrow. Fruit flies like a banana.",
    "I used to hate facial hair, but then it grew on me.",
    "What's the best thing about Switzerland? I don't know, but the flag is a big plus.",
    "Why don't eggs tell jokes? Because they'd crack each other up.",
    "I'm on a seafood diet. I see food and I eat it."
};

static const std::vector<std::string> pun_jokes = {
    "I used to be a banker, but I lost interest.",
    "I'm reading a book about mazes. I got lost in it.",
    "I tried to come up with a joke about clocks, but I didn't have the time.",
    "Did you hear about the guy who invented Lifesavers? He made a mint.",
    "I'm terrible at chess. It's really ruining my game.",
    "I would tell you a joke about construction, but I'm still working on it.",
    "My wife said I should do lunges to stay in shape. That would be a big step forward.",
    "Why was the belt arrested? For holding up a pair of pants.",
    "I was going to tell a joke about paper, but it's tearable.",
    "I used to play piano by ear, but now I use my hands.",
    "Pencils could be made with erasers on both ends, but what would be the point?",
    "I'm writing a book about reverse psychology. Don't read it.",
    "Why did the scarecrow win an award? He was outstanding in his field.",
    "The rotation of the earth really makes my day."
};

ToolResult JokeTool::execute(const std::map<std::string, std::string>& params) {
    auto cat_it = params.find("category");
    std::string category = (cat_it != params.end()) ? cat_it->second : "random";
    std::transform(category.begin(), category.end(), category.begin(), ::tolower);

    // Seed RNG with current time for true randomness each call
    auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937 rng(static_cast<unsigned int>(seed));

    const std::vector<std::string>* pool = nullptr;

    if (category == "programming") {
        pool = &programming_jokes;
    } else if (category == "pun") {
        pool = &pun_jokes;
    } else if (category == "general") {
        pool = &general_jokes;
    } else {
        // Random: pick from all pools
        std::vector<std::string> all;
        all.insert(all.end(), programming_jokes.begin(), programming_jokes.end());
        all.insert(all.end(), general_jokes.begin(), general_jokes.end());
        all.insert(all.end(), pun_jokes.begin(), pun_jokes.end());
        std::uniform_int_distribution<int> dist(0, static_cast<int>(all.size()) - 1);
        return {true, all[dist(rng)], ""};
    }

    std::uniform_int_distribution<int> dist(0, static_cast<int>(pool->size()) - 1);
    return {true, (*pool)[dist(rng)], ""};
}

} // namespace Jarvis
