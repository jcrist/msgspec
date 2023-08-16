module default {
  type Person {
    required name: str;
  }

  type Movie {
    required title: str;
    multi actors: Person;
  }
};
